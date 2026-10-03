#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "../../Common/Protocol/FrameDatagram.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kSampleMonitorId[] = L"DELD0E6";
constexpr std::uint16_t kPort = 48000;
constexpr std::uint16_t kOutputWidth = 160;
constexpr std::uint16_t kOutputHeight = 90;
constexpr DWORD kFrameIntervalMilliseconds = 100;

bool ContainsIgnoreCase(const std::wstring& value, const std::wstring& fragment)
{
    std::wstring upperValue = value;
    std::wstring upperFragment = fragment;
    std::transform(upperValue.begin(), upperValue.end(), upperValue.begin(), towupper);
    std::transform(upperFragment.begin(), upperFragment.end(), upperFragment.begin(), towupper);
    return upperValue.find(upperFragment) != std::wstring::npos;
}

bool FindSampleDisplay(std::wstring& displayName, std::wstring& friendlyName)
{
    UINT matchCount = 0;
    for (DWORD displayIndex = 0;; ++displayIndex)
    {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, displayIndex, &display, 0))
        {
            break;
        }
        if ((display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
        {
            continue;
        }

        for (DWORD monitorIndex = 0;; ++monitorIndex)
        {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(display.DeviceName, monitorIndex, &monitor, EDD_GET_DEVICE_INTERFACE_NAME))
            {
                break;
            }
            if (ContainsIgnoreCase(monitor.DeviceID, kSampleMonitorId))
            {
                ++matchCount;
                displayName = display.DeviceName;
                friendlyName = monitor.DeviceString;
            }
        }
    }
    return matchCount == 1;
}

bool DownscaleFrame(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                    ID3D11Texture2D* staging, std::vector<std::uint8_t>& output)
{
    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        std::wcerr << L"Unsupported source format: " << sourceDescription.Format << L'\n';
        return false;
    }

    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        std::wcerr << L"Could not map the staging surface: 0x" << std::hex << result << L'\n';
        return false;
    }

    output.resize(static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4);
    const auto* sourcePixels = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t y = 0; y < kOutputHeight; ++y)
    {
        const std::uint32_t sourceY = ((2 * y + 1) * sourceDescription.Height) / (2 * kOutputHeight);
        const auto* sourceRow = sourcePixels + static_cast<std::size_t>(sourceY) * mapped.RowPitch;
        auto* outputRow = output.data() + static_cast<std::size_t>(y) * kOutputWidth * 4;
        for (std::uint32_t x = 0; x < kOutputWidth; ++x)
        {
            const std::uint32_t sourceX = ((2 * x + 1) * sourceDescription.Width) / (2 * kOutputWidth);
            std::memcpy(outputRow + static_cast<std::size_t>(x) * 4, sourceRow + static_cast<std::size_t>(sourceX) * 4, 4);
        }
    }

    context->Unmap(staging, 0);
    return true;
}

bool SendFrame(SOCKET socketHandle, const sockaddr_in& destination, const std::vector<std::uint8_t>& pixels,
               std::uint64_t frameId)
{
    constexpr std::size_t frameBytes = static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4;
    constexpr std::uint16_t fragmentCount = static_cast<std::uint16_t>(
        (frameBytes + mydisplay::protocol::kMaxFragmentPayloadBytes - 1) /
        mydisplay::protocol::kMaxFragmentPayloadBytes);
    std::array<char, mydisplay::protocol::kMaxUdpDatagramBytes> datagram{};

    for (std::uint16_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex)
    {
        const std::size_t offset = static_cast<std::size_t>(fragmentIndex) * mydisplay::protocol::kMaxFragmentPayloadBytes;
        const std::size_t payloadBytes = (std::min)(mydisplay::protocol::kMaxFragmentPayloadBytes, frameBytes - offset);

        mydisplay::protocol::FrameDatagramHeader header{};
        header.magic = mydisplay::protocol::kFrameMagic;
        header.version = mydisplay::protocol::kProtocolVersion;
        header.headerBytes = sizeof(header);
        header.width = kOutputWidth;
        header.height = kOutputHeight;
        header.pixelFormat = mydisplay::protocol::kPixelFormatBgra8;
        header.frameId = frameId;
        header.fragmentIndex = fragmentIndex;
        header.fragmentCount = fragmentCount;
        header.payloadBytes = static_cast<std::uint16_t>(payloadBytes);

        std::memcpy(datagram.data(), &header, sizeof(header));
        std::memcpy(datagram.data() + sizeof(header), pixels.data() + offset, payloadBytes);
        const int bytesToSend = static_cast<int>(sizeof(header) + payloadBytes);
        if (sendto(socketHandle, datagram.data(), bytesToSend, 0,
                   reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) != bytesToSend)
        {
            std::wcerr << L"UDP send failed: " << WSAGetLastError() << L'\n';
            return false;
        }
    }
    return true;
}
}

int wmain(int argc, wchar_t* argv[])
{
    const wchar_t* destinationText = argc > 1 ? argv[1] : L"127.0.0.1";
    const unsigned long portValue = argc > 2 ? std::wcstoul(argv[2], nullptr, 10) : kPort;
    const bool allowInsecureRemoteTest = argc > 3 && _wcsicmp(argv[3], L"--allow-insecure-remote-test") == 0;
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

    const std::uint32_t destinationAddress = ntohl(destination.sin_addr.s_addr);
    const bool isLoopback = (destinationAddress & 0xFF000000) == 0x7F000000;
    if (!isLoopback && !allowInsecureRemoteTest)
    {
        std::wcerr << L"Unauthenticated test video is loopback-only. Remote testing requires the explicit "
                   << L"--allow-insecure-remote-test flag and a trusted isolated LAN.\n";
        closesocket(socketHandle);
        WSACleanup();
        return 2;
    }
    if (!isLoopback)
    {
        std::wcerr << L"WARNING: sending unencrypted, unauthenticated test frames to a remote host.\n";
    }

    std::wstring targetDisplayName;
    std::wstring friendlyName;
    if (!FindSampleDisplay(targetDisplayName, friendlyName))
    {
        std::wcerr << L"Expected exactly one active sample monitor with PnP ID " << kSampleMonitorId << L".\n";
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    std::wcout << L"Virtual display: " << friendlyName << L" (" << targetDisplayName << L")\n";
    std::wcout << L"Sending downscaled updates to " << destinationText << L':' << portValue
               << L" at up to 10 FPS. Press Ctrl+C to stop.\n";

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        std::wcerr << L"CreateDXGIFactory1 failed: 0x" << std::hex << hr << L'\n';
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput> selectedOutput;
    UINT outputMatches = 0;
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapterIndex, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(hr))
        {
            std::wcerr << L"EnumAdapters1 failed: 0x" << std::hex << hr << L'\n';
            closesocket(socketHandle);
            WSACleanup();
            return 1;
        }

        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIndex, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(hr))
            {
                std::wcerr << L"EnumOutputs failed: 0x" << std::hex << hr << L'\n';
                closesocket(socketHandle);
                WSACleanup();
                return 1;
            }

            DXGI_OUTPUT_DESC description{};
            hr = output->GetDesc(&description);
            if (SUCCEEDED(hr) && description.AttachedToDesktop &&
                _wcsicmp(description.DeviceName, targetDisplayName.c_str()) == 0)
            {
                ++outputMatches;
                selectedAdapter = adapter;
                selectedOutput = output;
            }
        }
    }
    if (outputMatches != 1)
    {
        std::wcerr << L"Expected one matching DXGI output; found " << outputMatches << L".\n";
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                           &device, nullptr, &context);
    if (FAILED(hr))
    {
        std::wcerr << L"D3D11CreateDevice failed: 0x" << std::hex << hr << L'\n';
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    ComPtr<IDXGIOutput1> output1;
    hr = selectedOutput.As(&output1);
    if (FAILED(hr))
    {
        std::wcerr << L"IDXGIOutput1 unavailable: 0x" << std::hex << hr << L'\n';
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }
    ComPtr<IDXGIOutputDuplication> duplication;
    hr = output1->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(hr))
    {
        std::wcerr << L"DuplicateOutput failed: 0x" << std::hex << hr << L'\n';
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    ComPtr<ID3D11Texture2D> staging;
    std::vector<std::uint8_t> outputPixels;
    std::uint64_t frameId = 0;
    ULONGLONG lastSent = 0;
    for (;;)
    {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        hr = duplication->AcquireNextFrame(500, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            continue;
        }
        if (FAILED(hr))
        {
            std::wcerr << L"AcquireNextFrame failed: 0x" << std::hex << hr << L'\n';
            break;
        }

        ComPtr<ID3D11Texture2D> source;
        hr = resource.As(&source);
        if (FAILED(hr))
        {
            duplication->ReleaseFrame();
            std::wcerr << L"Frame resource is not a D3D11 texture: 0x" << std::hex << hr << L'\n';
            break;
        }

        D3D11_TEXTURE2D_DESC sourceDescription{};
        source->GetDesc(&sourceDescription);
        if (!staging)
        {
            if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                duplication->ReleaseFrame();
                std::wcerr << L"Unsupported source format: " << sourceDescription.Format << L'\n';
                break;
            }
            D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.BindFlags = 0;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDescription.MiscFlags = 0;
            hr = device->CreateTexture2D(&stagingDescription, nullptr, &staging);
            if (FAILED(hr))
            {
                duplication->ReleaseFrame();
                std::wcerr << L"Could not create the staging texture: 0x" << std::hex << hr << L'\n';
                break;
            }
        }

        bool frameReady = true;
        bool shouldSend = false;
        const ULONGLONG now = GetTickCount64();
        if (lastSent == 0 || now - lastSent >= kFrameIntervalMilliseconds)
        {
            frameReady = DownscaleFrame(context.Get(), source.Get(), staging.Get(), outputPixels);
            shouldSend = frameReady;
        }
        const HRESULT releaseResult = duplication->ReleaseFrame();
        if (!frameReady || FAILED(releaseResult))
        {
            std::wcerr << L"Frame processing failed.\n";
            break;
        }

        if (shouldSend)
        {
            if (!SendFrame(socketHandle, destination, outputPixels, ++frameId))
            {
                break;
            }
            lastSent = now;
        }
    }

    closesocket(socketHandle);
    WSACleanup();
    return 1;
}