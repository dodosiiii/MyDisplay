#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kSampleMonitorId[] = L"DELD0E6";

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

            if (!ContainsIgnoreCase(monitor.DeviceID, kSampleMonitorId))
            {
                continue;
            }

            ++matchCount;
            displayName = display.DeviceName;
            friendlyName = monitor.DeviceString;
        }
    }

    return matchCount == 1;
}

bool SaveBitmap(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const std::wstring& path)
{
    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        std::wcerr << L"Unsupported Desktop Duplication pixel format: " << sourceDescription.Format << L'\n';
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&stagingDescription, nullptr, &staging);
    if (FAILED(result))
    {
        std::wcerr << L"CreateTexture2D for CPU readback failed: 0x" << std::hex << result << L'\n';
        return false;
    }

    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        std::wcerr << L"Mapping the staging texture failed: 0x" << std::hex << result << L'\n';
        return false;
    }

    const std::uint64_t rowBytes = static_cast<std::uint64_t>(sourceDescription.Width) * 4;
    const std::uint64_t imageBytes = rowBytes * sourceDescription.Height;
    if (imageBytes > MAXDWORD - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER))
    {
        context->Unmap(staging.Get(), 0);
        std::wcerr << L"Frame is too large for a BMP file.\n";
        return false;
    }

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(imageBytes);

    BITMAPINFOHEADER bitmapHeader{};
    bitmapHeader.biSize = sizeof(bitmapHeader);
    bitmapHeader.biWidth = static_cast<LONG>(sourceDescription.Width);
    bitmapHeader.biHeight = static_cast<LONG>(sourceDescription.Height);
    bitmapHeader.biPlanes = 1;
    bitmapHeader.biBitCount = 32;
    bitmapHeader.biCompression = BI_RGB;
    bitmapHeader.biSizeImage = static_cast<DWORD>(imageBytes);

    std::ofstream bitmap(path, std::ios::binary);
    if (!bitmap)
    {
        context->Unmap(staging.Get(), 0);
        std::wcerr << L"Could not open the output bitmap.\n";
        return false;
    }

    bitmap.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    bitmap.write(reinterpret_cast<const char*>(&bitmapHeader), sizeof(bitmapHeader));
    const auto* pixels = static_cast<const std::uint8_t*>(mapped.pData);
    for (UINT row = sourceDescription.Height; row > 0; --row)
    {
        const auto* rowData = pixels + static_cast<std::size_t>(row - 1) * mapped.RowPitch;
        bitmap.write(reinterpret_cast<const char*>(rowData), static_cast<std::streamsize>(rowBytes));
    }

    context->Unmap(staging.Get(), 0);
    if (!bitmap)
    {
        std::wcerr << L"Writing the output bitmap failed.\n";
        return false;
    }

    return true;
}
}

int wmain()
{
    std::wstring targetDisplayName;
    std::wstring friendlyName;
    if (!FindSampleDisplay(targetDisplayName, friendlyName))
    {
        std::wcerr << L"Expected exactly one active Microsoft sample monitor with PnP ID "
                   << kSampleMonitorId << L". No other display will be captured.\n";
        return 1;
    }

    std::wcout << L"Sample monitor: " << friendlyName << L" (" << targetDisplayName << L")\n";

    ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        std::wcerr << L"CreateDXGIFactory1 failed: 0x" << std::hex << result << L'\n';
        return 1;
    }

    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput> selectedOutput;
    UINT outputMatches = 0;

    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(adapterIndex, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(result))
        {
            std::wcerr << L"EnumAdapters1 failed: 0x" << std::hex << result << L'\n';
            return 1;
        }

        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            result = adapter->EnumOutputs(outputIndex, &output);
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                std::wcerr << L"EnumOutputs failed: 0x" << std::hex << result << L'\n';
                return 1;
            }

            DXGI_OUTPUT_DESC description{};
            result = output->GetDesc(&description);
            if (FAILED(result) || !description.AttachedToDesktop ||
                _wcsicmp(description.DeviceName, targetDisplayName.c_str()) != 0)
            {
                continue;
            }

            ++outputMatches;
            selectedAdapter = adapter;
            selectedOutput = output;
            std::wcout << L"DXGI output: " << description.DeviceName << L" ("
                       << description.DesktopCoordinates.right - description.DesktopCoordinates.left << L"x"
                       << description.DesktopCoordinates.bottom - description.DesktopCoordinates.top << L")\n";
        }
    }

    if (outputMatches != 1)
    {
        std::wcerr << L"Expected exactly one DXGI output for the verified sample monitor; found "
                   << outputMatches << L". Capture cancelled.\n";
        return 1;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    result = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                               &device, nullptr, &context);
    if (FAILED(result))
    {
        std::wcerr << L"D3D11CreateDevice failed: 0x" << std::hex << result << L'\n';
        return 1;
    }

    ComPtr<IDXGIOutput1> output1;
    result = selectedOutput.As(&output1);
    if (FAILED(result))
    {
        std::wcerr << L"IDXGIOutput1 is unavailable: 0x" << std::hex << result << L'\n';
        return 1;
    }

    ComPtr<IDXGIOutputDuplication> duplication;
    result = output1->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(result))
    {
        std::wcerr << L"DuplicateOutput failed: 0x" << std::hex << result << L'\n';
        return 1;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    const DWORD directoryLength = GetTempPathW(MAX_PATH, temporaryDirectory);
    if (directoryLength == 0 || directoryLength >= MAX_PATH)
    {
        std::wcerr << L"Could not determine the temporary directory.\n";
        return 1;
    }

    const std::wstring bitmapPath = std::wstring(temporaryDirectory) + L"MyDisplay-Phase2.bmp";
    constexpr ULONGLONG captureDurationMilliseconds = 10000;
    constexpr DWORD acquireTimeoutMilliseconds = 250;
    const ULONGLONG captureStart = GetTickCount64();
    std::uint64_t acquiredFrames = 0;
    std::uint64_t sourceFrames = 0;
    bool savedPreview = false;

    std::wcout << L"Capturing for 10 seconds. Move the pointer or a window on the virtual monitor to generate updates.\n";
    while (GetTickCount64() - captureStart < captureDurationMilliseconds)
    {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        result = duplication->AcquireNextFrame(acquireTimeoutMilliseconds, &frameInfo, &resource);
        if (result == DXGI_ERROR_WAIT_TIMEOUT)
        {
            continue;
        }
        if (FAILED(result))
        {
            std::wcerr << L"AcquireNextFrame failed: 0x" << std::hex << result << L'\n';
            return 1;
        }

        ComPtr<ID3D11Texture2D> frame;
        result = resource.As(&frame);
        if (FAILED(result))
        {
            duplication->ReleaseFrame();
            std::wcerr << L"The acquired frame is not a D3D11 texture: 0x" << std::hex << result << L'\n';
            return 1;
        }

        bool saveSucceeded = true;
        if (!savedPreview)
        {
            saveSucceeded = SaveBitmap(device.Get(), context.Get(), frame.Get(), bitmapPath);
            savedPreview = saveSucceeded;
        }

        const HRESULT releaseResult = duplication->ReleaseFrame();
        if (!saveSucceeded)
        {
            return 1;
        }
        if (FAILED(releaseResult))
        {
            std::wcerr << L"ReleaseFrame failed: 0x" << std::hex << releaseResult << L'\n';
            return 1;
        }

        ++acquiredFrames;
        sourceFrames += std::max<UINT>(1, frameInfo.AccumulatedFrames);
    }

    if (!savedPreview)
    {
        std::wcerr << L"No frame arrived during the capture window. Check that the virtual display is active.\n";
        return 1;
    }

    const double elapsedSeconds = static_cast<double>(GetTickCount64() - captureStart) / 1000.0;
    const double sourceFps = static_cast<double>(sourceFrames) / elapsedSeconds;
    std::wcout << L"Captured updates: " << acquiredFrames << L"; source frames reported: " << sourceFrames
               << L"; average source FPS: " << sourceFps << L"\n";
    std::wcout << L"Preview frame: " << bitmapPath << L'\n';
    return 0;
}