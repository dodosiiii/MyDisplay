#include <winsock2.h>
#include <ws2tcpip.h>

#include "../../Common/Protocol/FrameDatagram.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
constexpr std::uint16_t kWidth = 160;
constexpr std::uint16_t kHeight = 90;
constexpr std::uint16_t kPort = 48000;
constexpr unsigned int kFramesPerSecond = 10;

std::vector<std::uint8_t> MakeTestFrame(std::uint64_t frameId)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (std::uint32_t y = 0; y < kHeight; ++y)
    {
        for (std::uint32_t x = 0; x < kWidth; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            const auto moving = static_cast<std::uint8_t>((x + frameId * 3) % 256);
            pixels[offset] = static_cast<std::uint8_t>((y * 2 + frameId) % 256);
            pixels[offset + 1] = moving;
            pixels[offset + 2] = static_cast<std::uint8_t>((x * 255) / kWidth);
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}
}

int wmain(int argc, wchar_t* argv[])
{
    const wchar_t* destinationText = argc > 1 ? argv[1] : L"127.0.0.1";
    const unsigned long portValue = argc > 2 ? std::wcstoul(argv[2], nullptr, 10) : kPort;
    if (portValue == 0 || portValue > 65535)
    {
        std::wcerr << L"Port must be between 1 and 65535.\n";
        return 2;
    }

    WSADATA winsockData{};
    int result = WSAStartup(MAKEWORD(2, 2), &winsockData);
    if (result != 0)
    {
        std::wcerr << L"WSAStartup failed: " << result << L'\n';
        return 1;
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
    {
        std::wcerr << L"socket failed: " << WSAGetLastError() << L'\n';
        WSACleanup();
        return 1;
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<u_short>(portValue));
    if (InetPtonW(AF_INET, destinationText, &destination.sin_addr) != 1)
    {
        std::wcerr << L"Destination must be an IPv4 address.\n";
        closesocket(socketHandle);
        WSACleanup();
        return 2;
    }

    constexpr std::size_t frameBytes = static_cast<std::size_t>(kWidth) * kHeight * 4;
    constexpr std::uint16_t fragmentCount = static_cast<std::uint16_t>(
        (frameBytes + mydisplay::protocol::kMaxFragmentPayloadBytes - 1) /
        mydisplay::protocol::kMaxFragmentPayloadBytes);
    std::array<char, mydisplay::protocol::kMaxUdpDatagramBytes> datagram{};

    std::wcout << L"Sending a 160x90, 10 FPS test pattern to " << destinationText << L':' << portValue
               << L". Press Ctrl+C to stop.\n";

    for (std::uint64_t frameId = 1;; ++frameId)
    {
        const auto pixels = MakeTestFrame(frameId);
        for (std::uint16_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex)
        {
            const std::size_t offset = static_cast<std::size_t>(fragmentIndex) *
                                       mydisplay::protocol::kMaxFragmentPayloadBytes;
            const std::size_t payloadBytes = (std::min)(
                mydisplay::protocol::kMaxFragmentPayloadBytes, frameBytes - offset);

            mydisplay::protocol::FrameDatagramHeader header{};
            header.magic = mydisplay::protocol::kFrameMagic;
            header.version = mydisplay::protocol::kProtocolVersion;
            header.headerBytes = sizeof(header);
            header.width = kWidth;
            header.height = kHeight;
            header.pixelFormat = mydisplay::protocol::kPixelFormatBgra8;
            header.frameId = frameId;
            header.fragmentIndex = fragmentIndex;
            header.fragmentCount = fragmentCount;
            header.payloadBytes = static_cast<std::uint16_t>(payloadBytes);

            std::memcpy(datagram.data(), &header, sizeof(header));
            std::memcpy(datagram.data() + sizeof(header), pixels.data() + offset, payloadBytes);
            const int bytesToSend = static_cast<int>(sizeof(header) + payloadBytes);
            const int bytesSent = sendto(socketHandle, datagram.data(), bytesToSend, 0,
                                         reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
            if (bytesSent != bytesToSend)
            {
                std::wcerr << L"sendto failed: " << WSAGetLastError() << L'\n';
                closesocket(socketHandle);
                WSACleanup();
                return 1;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kFramesPerSecond));
    }
}