#pragma once

#include <winsock2.h>
#include <Windows.h>
#include <security.h>
#include <schannel.h>

#include <cstdint>
#include <string>
#include <vector>

class SchannelTlsStream
{
public:
    SchannelTlsStream() = default;
    SchannelTlsStream(const SchannelTlsStream&) = delete;
    SchannelTlsStream& operator=(const SchannelTlsStream&) = delete;
    ~SchannelTlsStream();

    bool Connect(SOCKET socket, const std::wstring& serverName, const std::wstring& expectedSha256);
    bool Accept(SOCKET socket, PCCERT_CONTEXT serverCertificate);
    bool SendAll(const void* data, std::size_t byteCount);
    bool ReceiveExact(void* data, std::size_t byteCount);

private:
    bool AcquireCredentials(bool server, PCCERT_CONTEXT serverCertificate);
    bool PerformHandshake(bool server, const std::wstring& serverName);
    bool SendHandshakeToken(const SecBuffer& token);
    bool ReceiveSocketBytes(std::vector<std::uint8_t>& buffer);
    bool VerifyRemoteCertificate(const std::wstring& expectedSha256);
    bool DecryptAvailableData();
    void Reset();

    SOCKET socket_ = INVALID_SOCKET;
    CredHandle credentials_{};
    CtxtHandle context_{};
    bool credentialsReady_ = false;
    bool contextReady_ = false;
    SecPkgContext_StreamSizes streamSizes_{};
    std::vector<std::uint8_t> encryptedInput_;
    std::vector<std::uint8_t> plaintext_;
    std::size_t plaintextOffset_ = 0;
};