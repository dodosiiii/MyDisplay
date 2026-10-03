#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <conio.h>

#include "../../Common/Protocol/FrameDatagram.h"
#include "../../Common/Protocol/SecureSession.h"
#include "../../Common/Security/SchannelTlsStream.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Credui.lib")

namespace
{
constexpr std::uint16_t kLocalUiPort = 48000;

std::wstring Sha256Fingerprint(PCCERT_CONTEXT certificate)
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

bool PromptForPin(std::string& pin)
{
    std::wcout << L"Enter the six-digit PIN shown by the server (input is masked): ";
    while (pin.size() < mydisplay::protocol::kPairingPinLength)
    {
        const int key = _getch();
        if (key == '\r')
        {
            break;
        }
        if (key == '\b')
        {
            if (!pin.empty())
            {
                pin.pop_back();
                std::wcout << L"\b \b";
            }
            continue;
        }
        if (key >= '0' && key <= '9')
        {
            pin.push_back(static_cast<char>(key));
            std::wcout << L'*';
        }
    }
    std::wcout << L'\n';
    return pin.size() == mydisplay::protocol::kPairingPinLength;
}

bool ReadPinFromStdin(std::string& pin)
{
    if (!std::getline(std::cin, pin))
    {
        return false;
    }
    if (!pin.empty() && pin.back() == '\r')
    {
        pin.pop_back();
    }
    return pin.size() == mydisplay::protocol::kPairingPinLength &&
           std::all_of(pin.begin(), pin.end(), [](char digit) { return digit >= '0' && digit <= '9'; });
}

bool ForwardFrames(SchannelTlsStream& tls, SOCKET localSocket)
{
    for (;;)
    {
        mydisplay::protocol::FrameDatagramHeader header{};
        if (!tls.ReceiveExact(&header, sizeof(header)))
        {
            std::wcerr << L"Secure video connection closed.\n";
            return false;
        }

        if (!mydisplay::protocol::IsValid(header,
            sizeof(header) + static_cast<std::size_t>(header.payloadBytes)))
        {
            std::wcerr << L"Rejected malformed secure frame packet.\n";
            return false;
        }

        std::array<char, mydisplay::protocol::kMaxUdpDatagramBytes> datagram{};
        std::memcpy(datagram.data(), &header, sizeof(header));
        if (!tls.ReceiveExact(datagram.data() + sizeof(header), header.payloadBytes))
        {
            return false;
        }

        const int datagramBytes = static_cast<int>(sizeof(header) + header.payloadBytes);
        sockaddr_in localAddress{};
        localAddress.sin_family = AF_INET;
        localAddress.sin_port = htons(kLocalUiPort);
        localAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (sendto(localSocket, datagram.data(), datagramBytes, 0,
                   reinterpret_cast<const sockaddr*>(&localAddress), sizeof(localAddress)) != datagramBytes)
        {
            std::wcerr << L"Could not forward a secure frame to the local viewer: " << WSAGetLastError() << L'\n';
            return false;
        }
    }
}
}

int wmain(int argc, wchar_t* argv[])
{
    const bool readPinFromStdin = argc == 6 && _wcsicmp(argv[5], L"--pin-stdin") == 0;
#ifdef _DEBUG
    in_addr parsedServerAddress{};
    const bool validServerAddress = argc > 1 && InetPtonW(AF_INET, argv[1], &parsedServerAddress) == 1;
    const bool isLoopback = validServerAddress && (ntohl(parsedServerAddress.s_addr) & 0xFF000000) == 0x7F000000;
    const bool useTestPin = argc == 7 && isLoopback && _wcsicmp(argv[5], L"--test-pin") == 0;
    if ((!useTestPin && !readPinFromStdin && argc != 5) || (!isLoopback && argc == 7))
#else
    if (!readPinFromStdin && argc != 5)
#endif
    {
        std::wcerr << L"Usage: DisplayClientTlsBridge.exe <server-ipv4> <server-name> <server.cer> <port>\n";
        return 2;
    }

    const unsigned long portValue = std::wcstoul(argv[4], nullptr, 10);
    if (portValue == 0 || portValue > 65535)
    {
        std::wcerr << L"Port must be between 1 and 65535.\n";
        return 2;
    }

    HANDLE certificateFile = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (certificateFile == INVALID_HANDLE_VALUE)
    {
        std::wcerr << L"Could not open the pinned server .cer file.\n";
        return 1;
    }
    const DWORD certificateBytesCount = GetFileSize(certificateFile, nullptr);
    if (certificateBytesCount == INVALID_FILE_SIZE || certificateBytesCount == 0 || certificateBytesCount > 1024 * 1024)
    {
        CloseHandle(certificateFile);
        std::wcerr << L"Invalid server certificate file size.\n";
        return 1;
    }
    std::vector<BYTE> certificateBytes(certificateBytesCount);
    DWORD bytesRead = 0;
    const BOOL fileRead = ReadFile(certificateFile, certificateBytes.data(), certificateBytesCount, &bytesRead, nullptr);
    CloseHandle(certificateFile);
    if (!fileRead || bytesRead != certificateBytesCount)
    {
        std::wcerr << L"Could not read the server certificate file.\n";
        return 1;
    }

    PCCERT_CONTEXT certificate = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, certificateBytes.data(), certificateBytesCount);
    if (!certificate)
    {
        std::wcerr << L"The server certificate file is invalid.\n";
        return 1;
    }
    const std::wstring fingerprint = Sha256Fingerprint(certificate);
    CertFreeCertificateContext(certificate);
    if (fingerprint.empty())
    {
        std::wcerr << L"Could not calculate the server certificate SHA-256 pin.\n";
        return 1;
    }

    std::wstring serverName = argv[2];
    std::string pin;
#ifdef _DEBUG
    if (useTestPin)
    {
        const std::wstring testPin = argv[6];
        if (testPin.size() != mydisplay::protocol::kPairingPinLength ||
            !std::all_of(testPin.begin(), testPin.end(), [](wchar_t digit) { return digit >= L'0' && digit <= L'9'; }))
        {
            std::wcerr << L"Debug loopback test PIN must contain six digits.\n";
            return 2;
        }
        pin.clear();
        for (const wchar_t digit : testPin)
        {
            pin.push_back(static_cast<char>(digit));
        }
    }
    else if (!(readPinFromStdin ? ReadPinFromStdin(pin) : PromptForPin(pin)))
#else
    if (!(readPinFromStdin ? ReadPinFromStdin(pin) : PromptForPin(pin)))
#endif
    {
        std::wcerr << L"Connection cancelled or PIN must contain six digits.\n";
        return 2;
    }

    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0)
    {
        return 1;
    }
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(static_cast<u_short>(portValue));
    if (socketHandle == INVALID_SOCKET || InetPtonW(AF_INET, argv[1], &serverAddress.sin_addr) != 1 ||
        connect(socketHandle, reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)) == SOCKET_ERROR)
    {
        std::wcerr << L"Could not connect to the server: " << WSAGetLastError() << L'\n';
        if (socketHandle != INVALID_SOCKET) closesocket(socketHandle);
        WSACleanup();
        return 1;
    }
    const BOOL noDelay = TRUE;
    setsockopt(socketHandle, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    SchannelTlsStream tls;
    if (!tls.Connect(socketHandle, serverName, fingerprint))
    {
        std::wcerr << L"TLS handshake or certificate pin verification failed.\n";
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    mydisplay::protocol::PairingRequest request{};
    request.magic = mydisplay::protocol::kPairingMagic;
    request.version = mydisplay::protocol::kSecureProtocolVersion;
    request.pinLength = mydisplay::protocol::kPairingPinLength;
    std::memcpy(request.pin, pin.data(), pin.size());
    SecureZeroMemory(pin.data(), pin.size());

    mydisplay::protocol::PairingResponse response{};
    if (!tls.SendAll(&request, sizeof(request)) || !tls.ReceiveExact(&response, sizeof(response)) ||
        response.version != mydisplay::protocol::kSecureProtocolVersion || response.status != 0 ||
        response.magic != mydisplay::protocol::kPairingAcceptedMagic)
    {
        std::wcerr << L"The server rejected this client or the PIN was incorrect.\n";
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    std::wcout << L"TLS certificate pin verified and session PIN accepted. Starting the local viewer bridge.\n";
    SOCKET localSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (localSocket == INVALID_SOCKET)
    {
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    ForwardFrames(tls, localSocket);
    closesocket(localSocket);
    closesocket(socketHandle);
    WSACleanup();
    return 0;
}