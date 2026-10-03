#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include "../Protocol/FrameDatagram.h"
#include "../Protocol/SecureSession.h"
#include "SchannelTlsStream.h"

#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace
{
constexpr u_short kPort = 49203;
constexpr char kLoopbackTestPin[] = "314159";

bool IsValidPairingPin(const mydisplay::protocol::PairingRequest& request)
{
    if (request.magic != mydisplay::protocol::kPairingMagic ||
        request.version != mydisplay::protocol::kSecureProtocolVersion ||
        request.pinLength != mydisplay::protocol::kPairingPinLength || request.reserved != 0)
    {
        return false;
    }

    unsigned char difference = 0;
    for (std::size_t index = 0; index < mydisplay::protocol::kPairingPinLength; ++index)
    {
        difference |= static_cast<unsigned char>(request.pin[index] ^ kLoopbackTestPin[index]);
    }
    return difference == 0;
}

PCCERT_CONTEXT FindServerCertificate()
{
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD computerNameLength = ARRAYSIZE(computerName);
    if (!GetComputerNameW(computerName, &computerNameLength))
    {
        return nullptr;
    }

    const std::wstring subject = L"MyDisplayServer-" + std::wstring(computerName, computerNameLength);
    HCERTSTORE store = CertOpenSystemStoreW(0, L"MY");
    if (!store)
    {
        return nullptr;
    }
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W,
        subject.c_str(), nullptr);
    CertCloseStore(store, 0);
    return certificate;
}

std::wstring GetSha256Fingerprint(PCCERT_CONTEXT certificate)
{
    std::array<BYTE, 32> hash{};
    DWORD hashLength = static_cast<DWORD>(hash.size());
    if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
                               certificate->pbCertEncoded, certificate->cbCertEncoded,
                               hash.data(), &hashLength) || hashLength != hash.size())
    {
        return {};
    }

    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring fingerprint;
    fingerprint.reserve(hashLength * 2);
    for (DWORD index = 0; index < hashLength; ++index)
    {
        fingerprint.push_back(hex[hash[index] >> 4]);
        fingerprint.push_back(hex[hash[index] & 0x0F]);
    }
    return fingerprint;
}

bool RunServer(SOCKET listener, PCCERT_CONTEXT certificate, bool& serverPassed)
{
    SOCKET clientSocket = accept(listener, nullptr, nullptr);
    if (clientSocket == INVALID_SOCKET)
    {
        std::cerr << "accept failed: " << WSAGetLastError() << '\n';
        return false;
    }

    SchannelTlsStream tls;
    if (!tls.Accept(clientSocket, certificate))
    {
        std::cerr << "Server-side TLS handshake failed.\n";
        closesocket(clientSocket);
        return false;
    }

    mydisplay::protocol::PairingRequest pairingRequest{};
    if (!tls.ReceiveExact(&pairingRequest, sizeof(pairingRequest)))
    {
        std::cerr << "TLS pairing request receive failed.\n";
        closesocket(clientSocket);
        return false;
    }

    const bool pinAccepted = IsValidPairingPin(pairingRequest);
    mydisplay::protocol::PairingResponse pairingResponse{};
    pairingResponse.magic = pinAccepted ? mydisplay::protocol::kPairingAcceptedMagic : mydisplay::protocol::kPairingRejectedMagic;
    pairingResponse.version = mydisplay::protocol::kSecureProtocolVersion;
    pairingResponse.status = pinAccepted ? 0 : 1;
    if (!tls.SendAll(&pairingResponse, sizeof(pairingResponse)) || !pinAccepted)
    {
        std::cerr << "TLS-protected PIN authentication failed.\n";
        closesocket(clientSocket);
        return false;
    }

    mydisplay::protocol::FrameDatagramHeader frameHeader{};
    std::array<std::uint8_t, 4> pixels{};
    if (!tls.ReceiveExact(&frameHeader, sizeof(frameHeader)) ||
        !mydisplay::protocol::IsValid(frameHeader, sizeof(frameHeader) + frameHeader.payloadBytes) ||
        !tls.ReceiveExact(pixels.data(), pixels.size()) || frameHeader.width != 1 || frameHeader.height != 1 ||
        frameHeader.payloadBytes != pixels.size())
    {
        std::cerr << "Encrypted frame payload validation failed.\n";
        closesocket(clientSocket);
        return false;
    }

    serverPassed = true;
    closesocket(clientSocket);
    return true;
}
}

int wmain()
{
    wchar_t localAppData[MAX_PATH]{};
    const DWORD localAppDataLength = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
    if (localAppDataLength == 0 || localAppDataLength >= ARRAYSIZE(localAppData))
    {
        std::wcerr << L"LOCALAPPDATA is unavailable.\n";
        return 1;
    }
    const std::wstring certificatePath = std::wstring(localAppData, localAppDataLength) +
                                         L"\\MyDisplay\\ServerCertificate.cer";
    PCCERT_CONTEXT publicCertificate = nullptr;
    std::vector<BYTE> certificateBytes;
    HANDLE certificateFile = CreateFileW(certificatePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (certificateFile == INVALID_HANDLE_VALUE)
    {
        std::wcerr << L"Run Installer\\New-ServerCertificate.ps1 first.\n";
        return 1;
    }
    const DWORD fileSize = GetFileSize(certificateFile, nullptr);
    certificateBytes.resize(fileSize);
    DWORD bytesRead = 0;
    const BOOL fileRead = ReadFile(certificateFile, certificateBytes.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(certificateFile);
    if (!fileRead || bytesRead != fileSize)
    {
        std::wcerr << L"Could not read the public certificate file.\n";
        return 1;
    }

    publicCertificate = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                      certificateBytes.data(), fileSize);
    if (!publicCertificate)
    {
        std::wcerr << L"Could not parse the public certificate file.\n";
        return 1;
    }
    const std::wstring fingerprint = GetSha256Fingerprint(publicCertificate);

    PCCERT_CONTEXT serverCertificate = FindServerCertificate();
    if (!serverCertificate || fingerprint.empty())
    {
        std::wcerr << L"Could not find the server certificate/private key or calculate its SHA-256 pin.\n";
        if (serverCertificate) CertFreeCertificateContext(serverCertificate);
        CertFreeCertificateContext(publicCertificate);
        return 1;
    }

    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0)
    {
        std::wcerr << L"WSAStartup failed.\n";
        CertFreeCertificateContext(serverCertificate);
        CertFreeCertificateContext(publicCertificate);
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR)
    {
        std::wcerr << L"Could not open the loopback TLS test listener: " << WSAGetLastError() << L'\n';
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        CertFreeCertificateContext(serverCertificate);
        CertFreeCertificateContext(publicCertificate);
        return 1;
    }

    bool serverPassed = false;
    std::thread serverThread(RunServer, listener, serverCertificate, std::ref(serverPassed));
    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const bool connected = clientSocket != INVALID_SOCKET &&
        connect(clientSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;

    bool clientPassed = false;
    if (connected)
    {
        wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD computerNameLength = ARRAYSIZE(computerName);
        GetComputerNameW(computerName, &computerNameLength);

        SchannelTlsStream tls;
        if (tls.Connect(clientSocket, computerName, fingerprint))
        {
            mydisplay::protocol::PairingRequest pairingRequest{};
            pairingRequest.magic = mydisplay::protocol::kPairingMagic;
            pairingRequest.version = mydisplay::protocol::kSecureProtocolVersion;
            pairingRequest.pinLength = mydisplay::protocol::kPairingPinLength;
            std::memcpy(pairingRequest.pin, kLoopbackTestPin, sizeof(kLoopbackTestPin) - 1);

            mydisplay::protocol::PairingResponse pairingResponse{};
            if (tls.SendAll(&pairingRequest, sizeof(pairingRequest)) &&
                tls.ReceiveExact(&pairingResponse, sizeof(pairingResponse)) &&
                pairingResponse.magic == mydisplay::protocol::kPairingAcceptedMagic &&
                pairingResponse.version == mydisplay::protocol::kSecureProtocolVersion &&
                pairingResponse.status == 0)
            {
                mydisplay::protocol::FrameDatagramHeader frameHeader{};
                frameHeader.magic = mydisplay::protocol::kFrameMagic;
                frameHeader.version = mydisplay::protocol::kProtocolVersion;
                frameHeader.headerBytes = sizeof(frameHeader);
                frameHeader.width = 1;
                frameHeader.height = 1;
                frameHeader.pixelFormat = mydisplay::protocol::kPixelFormatBgra8;
                frameHeader.frameId = 1;
                frameHeader.fragmentCount = 1;
                frameHeader.payloadBytes = 4;
                const std::array<std::uint8_t, 4> pixels{ 0x12, 0x34, 0x56, 0xFF };
                clientPassed = tls.SendAll(&frameHeader, sizeof(frameHeader)) &&
                               tls.SendAll(pixels.data(), pixels.size());
            }
        }
    }
    if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);

    serverThread.join();
    closesocket(listener);
    WSACleanup();
    CertFreeCertificateContext(serverCertificate);
    CertFreeCertificateContext(publicCertificate);

    std::wcout << L"TLS 1.2 server certificate pin: " << fingerprint << L'\n';
    std::wcout << L"Client exchange: " << (clientPassed ? L"PASS" : L"FAIL") << L'\n';
    std::wcout << L"Server exchange: " << (serverPassed ? L"PASS" : L"FAIL") << L'\n';
    return clientPassed && serverPassed ? 0 : 1;
}