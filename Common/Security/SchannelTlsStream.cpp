#include "SchannelTlsStream.h"

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace
{
constexpr std::size_t kSocketReadBytes = 16 * 1024;
constexpr DWORD kTlsClientFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                 ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                                 ISC_REQ_MANUAL_CRED_VALIDATION;
constexpr DWORD kTlsServerFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                                 ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;

bool SendSocketBytes(SOCKET socket, const std::uint8_t* bytes, std::size_t byteCount)
{
    while (byteCount > 0)
    {
        const int chunk = static_cast<int>((std::min)(byteCount, static_cast<std::size_t>(INT_MAX)));
        const int sent = send(socket, reinterpret_cast<const char*>(bytes), chunk, 0);
        if (sent == SOCKET_ERROR || sent == 0)
        {
            return false;
        }
        bytes += sent;
        byteCount -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool ReceiveSocketBytes(SOCKET socket, std::vector<std::uint8_t>& buffer)
{
    std::array<std::uint8_t, kSocketReadBytes> temporary{};
    const int received = recv(socket, reinterpret_cast<char*>(temporary.data()), static_cast<int>(temporary.size()), 0);
    if (received <= 0)
    {
        return false;
    }
    buffer.insert(buffer.end(), temporary.begin(), temporary.begin() + received);
    return true;
}

std::wstring NormalizeFingerprint(const std::wstring& fingerprint)
{
    std::wstring normalized;
    for (wchar_t character : fingerprint)
    {
        if (character == L':' || iswspace(character))
        {
            continue;
        }
        normalized.push_back(static_cast<wchar_t>(towupper(character)));
    }
    return normalized;
}

std::wstring Sha256Fingerprint(PCCERT_CONTEXT certificate)
{
    std::array<BYTE, 32> hash{};
    DWORD hashBytes = static_cast<DWORD>(hash.size());
    if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
                               certificate->pbCertEncoded, certificate->cbCertEncoded,
                               hash.data(), &hashBytes) || hashBytes != hash.size())
    {
        return {};
    }

    std::wostringstream value;
    value << std::uppercase << std::hex << std::setfill(L'0');
    for (DWORD index = 0; index < hashBytes; ++index)
    {
        value << std::setw(2) << static_cast<unsigned int>(hash[index]);
    }
    return value.str();
}

bool ExtractExtraBuffer(SecBuffer* buffers, ULONG bufferCount,
                        const std::vector<std::uint8_t>& input,
                        std::vector<std::uint8_t>& extra)
{
    for (ULONG index = 0; index < bufferCount; ++index)
    {
        if (buffers[index].BufferType == SECBUFFER_EXTRA)
        {
            const auto* start = static_cast<const std::uint8_t*>(buffers[index].pvBuffer);
            if (!start || start < input.data() || start > input.data() + input.size() ||
                buffers[index].cbBuffer > static_cast<ULONG>(input.data() + input.size() - start))
            {
                return false;
            }
            extra.assign(start, start + buffers[index].cbBuffer);
            return true;
        }
    }
    extra.clear();
    return true;
}
}

SchannelTlsStream::~SchannelTlsStream()
{
    Reset();
}

void SchannelTlsStream::Reset()
{
    if (contextReady_)
    {
        DeleteSecurityContext(&context_);
        contextReady_ = false;
    }
    if (credentialsReady_)
    {
        FreeCredentialsHandle(&credentials_);
        credentialsReady_ = false;
    }
    encryptedInput_.clear();
    plaintext_.clear();
    plaintextOffset_ = 0;
}

bool SchannelTlsStream::AcquireCredentials(bool server, PCCERT_CONTEXT serverCertificate)
{
    SCHANNEL_CRED schannelCredentials{};
    schannelCredentials.dwVersion = SCHANNEL_CRED_VERSION;
    schannelCredentials.grbitEnabledProtocols = server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT;

    if (server)
    {
        if (!serverCertificate)
        {
            return false;
        }
        schannelCredentials.cCreds = 1;
        schannelCredentials.paCred = &serverCertificate;
        schannelCredentials.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER;
    }
    else
    {
        schannelCredentials.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    }

    TimeStamp expiration{};
    const SECURITY_STATUS status = AcquireCredentialsHandleW(
        nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
        server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND,
        nullptr, &schannelCredentials, nullptr, nullptr, &credentials_, &expiration);
    credentialsReady_ = status == SEC_E_OK;
    return credentialsReady_;
}

bool SchannelTlsStream::Connect(SOCKET socket, const std::wstring& serverName, const std::wstring& expectedSha256)
{
    Reset();
    socket_ = socket;
    if (!AcquireCredentials(false, nullptr) || !PerformHandshake(false, serverName) ||
        !VerifyRemoteCertificate(expectedSha256))
    {
        Reset();
        return false;
    }

    return QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &streamSizes_) == SEC_E_OK;
}

bool SchannelTlsStream::Accept(SOCKET socket, PCCERT_CONTEXT serverCertificate)
{
    Reset();
    socket_ = socket;
    if (!AcquireCredentials(true, serverCertificate) || !PerformHandshake(true, L""))
    {
        Reset();
        return false;
    }

    return QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &streamSizes_) == SEC_E_OK;
}

bool SchannelTlsStream::SendHandshakeToken(const SecBuffer& token)
{
    return token.cbBuffer == 0 || (token.pvBuffer &&
        SendSocketBytes(socket_, static_cast<const std::uint8_t*>(token.pvBuffer), token.cbBuffer));
}

bool SchannelTlsStream::ReceiveSocketBytes(std::vector<std::uint8_t>& buffer)
{
    return ::ReceiveSocketBytes(socket_, buffer);
}

bool SchannelTlsStream::PerformHandshake(bool server, const std::wstring& serverName)
{
    std::vector<std::uint8_t> input;
    if (server && !ReceiveSocketBytes(input))
    {
        return false;
    }

    bool firstCall = true;
    for (;;)
    {
        SecBuffer inputBuffers[2]{};
        SecBufferDesc inputDescription{};
        SecBufferDesc* inputDescriptionPointer = nullptr;
        if (!input.empty())
        {
            inputBuffers[0].BufferType = SECBUFFER_TOKEN;
            inputBuffers[0].cbBuffer = static_cast<ULONG>(input.size());
            inputBuffers[0].pvBuffer = input.data();
            inputBuffers[1].BufferType = SECBUFFER_EMPTY;
            inputDescription.ulVersion = SECBUFFER_VERSION;
            inputDescription.cBuffers = ARRAYSIZE(inputBuffers);
            inputDescription.pBuffers = inputBuffers;
            inputDescriptionPointer = &inputDescription;
        }

        SecBuffer outputBuffer{};
        outputBuffer.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outputDescription{};
        outputDescription.ulVersion = SECBUFFER_VERSION;
        outputDescription.cBuffers = 1;
        outputDescription.pBuffers = &outputBuffer;
        ULONG contextAttributes = 0;
        TimeStamp expiration{};

        SECURITY_STATUS status;
        if (server)
        {
            status = AcceptSecurityContext(&credentials_, firstCall ? nullptr : &context_,
                inputDescriptionPointer, kTlsServerFlags, SECURITY_NATIVE_DREP,
                &context_, &outputDescription, &contextAttributes, &expiration);
        }
        else
        {
            status = InitializeSecurityContextW(&credentials_, firstCall ? nullptr : &context_,
                const_cast<wchar_t*>(serverName.c_str()), kTlsClientFlags, 0,
                SECURITY_NATIVE_DREP, inputDescriptionPointer, 0, &context_,
                &outputDescription, &contextAttributes, &expiration);
        }
        firstCall = false;
        contextReady_ = true;

        if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE)
        {
            if (CompleteAuthToken(&context_, &outputDescription) != SEC_E_OK)
            {
                if (outputBuffer.pvBuffer)
                {
                    FreeContextBuffer(outputBuffer.pvBuffer);
                }
                return false;
            }
            status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
        }

        const bool sent = SendHandshakeToken(outputBuffer);
        if (outputBuffer.pvBuffer)
        {
            FreeContextBuffer(outputBuffer.pvBuffer);
        }
        if (!sent)
        {
            return false;
        }

        std::vector<std::uint8_t> extra;
        if (inputDescriptionPointer && !ExtractExtraBuffer(inputBuffers, ARRAYSIZE(inputBuffers), input, extra))
        {
            return false;
        }
        input.swap(extra);

        if (status == SEC_E_OK)
        {
            encryptedInput_ = std::move(input);
            return true;
        }
        if (status != SEC_I_CONTINUE_NEEDED && status != SEC_E_INCOMPLETE_MESSAGE)
        {
            return false;
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE || input.empty())
        {
            if (!ReceiveSocketBytes(input))
            {
                return false;
            }
        }
    }
}

bool SchannelTlsStream::VerifyRemoteCertificate(const std::wstring& expectedSha256)
{
    PCCERT_CONTEXT remoteCertificate = nullptr;
    if (QueryContextAttributesW(&context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remoteCertificate) != SEC_E_OK ||
        !remoteCertificate)
    {
        return false;
    }

    const std::wstring actualFingerprint = Sha256Fingerprint(remoteCertificate);
    CertFreeCertificateContext(remoteCertificate);
    return !actualFingerprint.empty() && NormalizeFingerprint(actualFingerprint) == NormalizeFingerprint(expectedSha256);
}

bool SchannelTlsStream::SendAll(const void* data, std::size_t byteCount)
{
    const auto* input = static_cast<const std::uint8_t*>(data);
    while (byteCount > 0)
    {
        const std::size_t payloadBytes = (std::min)(byteCount, static_cast<std::size_t>(streamSizes_.cbMaximumMessage));
        const std::size_t totalBytes = streamSizes_.cbHeader + payloadBytes + streamSizes_.cbTrailer;
        std::vector<std::uint8_t> encrypted(totalBytes);
        std::memcpy(encrypted.data() + streamSizes_.cbHeader, input, payloadBytes);

        SecBuffer buffers[4]{};
        buffers[0] = { streamSizes_.cbHeader, SECBUFFER_STREAM_HEADER, encrypted.data() };
        buffers[1] = { static_cast<ULONG>(payloadBytes), SECBUFFER_DATA, encrypted.data() + streamSizes_.cbHeader };
        buffers[2] = { streamSizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, encrypted.data() + streamSizes_.cbHeader + payloadBytes };
        buffers[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc description{ SECBUFFER_VERSION, ARRAYSIZE(buffers), buffers };
        if (EncryptMessage(&context_, 0, &description, 0) != SEC_E_OK)
        {
            return false;
        }

        std::size_t encryptedBytes = 0;
        for (const auto& buffer : buffers)
        {
            if (buffer.BufferType == SECBUFFER_STREAM_HEADER || buffer.BufferType == SECBUFFER_DATA ||
                buffer.BufferType == SECBUFFER_STREAM_TRAILER)
            {
                encryptedBytes += buffer.cbBuffer;
            }
        }
        if (!SendSocketBytes(socket_, encrypted.data(), encryptedBytes))
        {
            return false;
        }
        input += payloadBytes;
        byteCount -= payloadBytes;
    }
    return true;
}

bool SchannelTlsStream::DecryptAvailableData()
{
    while (encryptedInput_.empty())
    {
        if (!ReceiveSocketBytes(encryptedInput_))
        {
            return false;
        }
    }

    for (;;)
    {
        SecBuffer buffers[4]{};
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].cbBuffer = static_cast<ULONG>(encryptedInput_.size());
        buffers[0].pvBuffer = encryptedInput_.data();
        for (std::size_t index = 1; index < ARRAYSIZE(buffers); ++index)
        {
            buffers[index].BufferType = SECBUFFER_EMPTY;
        }
        SecBufferDesc description{ SECBUFFER_VERSION, ARRAYSIZE(buffers), buffers };
        const SECURITY_STATUS status = DecryptMessage(&context_, &description, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            if (!ReceiveSocketBytes(encryptedInput_))
            {
                return false;
            }
            continue;
        }
        if (status != SEC_E_OK)
        {
            return false;
        }

        std::vector<std::uint8_t> extra;
        if (!ExtractExtraBuffer(buffers, ARRAYSIZE(buffers), encryptedInput_, extra))
        {
            return false;
        }
        for (const auto& buffer : buffers)
        {
            if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer > 0)
            {
                const auto* data = static_cast<const std::uint8_t*>(buffer.pvBuffer);
                plaintext_.insert(plaintext_.end(), data, data + buffer.cbBuffer);
            }
        }
        encryptedInput_.swap(extra);
        return true;
    }
}

bool SchannelTlsStream::ReceiveExact(void* data, std::size_t byteCount)
{
    auto* output = static_cast<std::uint8_t*>(data);
    while (byteCount > 0)
    {
        const std::size_t available = plaintext_.size() - plaintextOffset_;
        if (available == 0)
        {
            plaintext_.clear();
            plaintextOffset_ = 0;
            if (!DecryptAvailableData())
            {
                return false;
            }
            continue;
        }

        const std::size_t copyBytes = (std::min)(byteCount, available);
        std::memcpy(output, plaintext_.data() + plaintextOffset_, copyBytes);
        output += copyBytes;
        byteCount -= copyBytes;
        plaintextOffset_ += copyBytes;
    }
    return true;
}