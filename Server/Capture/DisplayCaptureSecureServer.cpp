#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <bcrypt.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include "../../Common/Protocol/FrameDatagram.h"
#include "../../Common/Protocol/SecureSession.h"
#include "../../Common/Security/SchannelTlsStream.h"
#include "../../Common/Video/H264Codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Crypt32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kSampleMonitorId[] = L"DELD0E6";
constexpr std::uint16_t kPort = 48001;
constexpr std::uint16_t kOutputWidth = 1920;
constexpr std::uint16_t kOutputHeight = 1080;
constexpr std::uint32_t kTargetFramesPerSecond = 60;
constexpr std::uint32_t kTargetBitrate = 50000000;

bool ContainsIgnoreCase(const std::wstring& value, const std::wstring& fragment)
{
    std::wstring upperValue = value;
    std::wstring upperFragment = fragment;
    std::transform(upperValue.begin(), upperValue.end(), upperValue.begin(), towupper);
    std::transform(upperFragment.begin(), upperFragment.end(), upperFragment.begin(), towupper);
    return upperValue.find(upperFragment) != std::wstring::npos;
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
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_SUBJECT_STR_W, subject.c_str(), nullptr);
    CertCloseStore(store, 0);
    return certificate;
}

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

    std::wostringstream value;
    value << std::uppercase << std::hex << std::setfill(L'0');
    for (DWORD index = 0; index < hashLength; ++index)
    {
        value << std::setw(2) << static_cast<unsigned int>(hash[index]);
    }
    return value.str();
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
            std::memcpy(outputRow + static_cast<std::size_t>(x) * 4,
                        sourceRow + static_cast<std::size_t>(sourceX) * 4, 4);
        }
    }
    context->Unmap(staging, 0);
    return true;
}

bool SendEncodedFrame(SchannelTlsStream& tls, const mydisplay::video::EncodedFrame& frame)
{
    if (frame.bytes.empty() || frame.bytes.size() > mydisplay::protocol::kMaxEncodedFrameBytes)
    {
        return false;
    }
    const std::size_t fragmentCountSize = (frame.bytes.size() +
        mydisplay::protocol::kMaxFragmentPayloadBytes - 1) /
        mydisplay::protocol::kMaxFragmentPayloadBytes;
    if (fragmentCountSize > (std::numeric_limits<std::uint16_t>::max)())
    {
        return false;
    }
    const auto fragmentCount = static_cast<std::uint16_t>(fragmentCountSize);
    std::array<char, mydisplay::protocol::kMaxUdpDatagramBytes> packet{};

    for (std::uint16_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex)
    {
        const std::size_t offset = static_cast<std::size_t>(fragmentIndex) *
                                   mydisplay::protocol::kMaxFragmentPayloadBytes;
        const std::size_t payloadBytes = (std::min)(
            mydisplay::protocol::kMaxFragmentPayloadBytes, frame.bytes.size() - offset);

        mydisplay::protocol::FrameDatagramHeader header{};
        header.magic = mydisplay::protocol::kFrameMagic;
        header.version = mydisplay::protocol::kProtocolVersion;
        header.headerBytes = sizeof(header);
        header.width = kOutputWidth;
        header.height = kOutputHeight;
        header.pixelFormat = mydisplay::protocol::kPixelFormatH264;
        header.frameId = frame.frameId;
        header.fragmentIndex = fragmentIndex;
        header.fragmentCount = fragmentCount;
        header.payloadBytes = static_cast<std::uint16_t>(payloadBytes);

        std::memcpy(packet.data(), &header, sizeof(header));
        std::memcpy(packet.data() + sizeof(header), frame.bytes.data() + offset, payloadBytes);
        if (!tls.SendAll(packet.data(), sizeof(header) + payloadBytes))
        {
            return false;
        }
    }
    return true;
}

bool RecreateDuplication(IDXGIOutput1* output, ID3D11Device* device,
                         ComPtr<IDXGIOutputDuplication>& duplication)
{
    duplication.Reset();
    const HRESULT result = output->DuplicateOutput(device, &duplication);
    if (FAILED(result))
    {
        std::wcerr << L"Could not recreate DXGI duplication after display change: 0x" << std::hex << result << L'\n';
        return false;
    }
    return true;
}

bool VerifyPin(const mydisplay::protocol::PairingRequest& request, const std::string& expectedPin)
{
    if (request.magic != mydisplay::protocol::kPairingMagic ||
        request.version != mydisplay::protocol::kSecureProtocolVersion ||
        request.pinLength != mydisplay::protocol::kPairingPinLength || request.reserved != 0 ||
        expectedPin.size() != mydisplay::protocol::kPairingPinLength)
    {
        return false;
    }

    unsigned char difference = 0;
    for (std::size_t index = 0; index < mydisplay::protocol::kPairingPinLength; ++index)
    {
        difference |= static_cast<unsigned char>(request.pin[index] ^ expectedPin[index]);
    }
    return difference == 0;
}

std::string CreateSessionPin()
{
    std::uint32_t randomValue = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&randomValue), sizeof(randomValue),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    {
        return {};
    }
    const unsigned int pinValue = randomValue % 1000000;
    std::string pin = std::to_string(pinValue);
    pin.insert(pin.begin(), 6 - pin.length(), '0');
    return pin;
}

bool ReadSessionPinFromStdin(std::string& pin)
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    pin.clear();
    pin.reserve(mydisplay::protocol::kPairingPinLength);
    while (pin.size() < mydisplay::protocol::kPairingPinLength)
    {
        char digit = 0;
        DWORD bytesRead = 0;
        if (!ReadFile(input, &digit, 1, &bytesRead, nullptr) || bytesRead != 1 ||
            digit < '0' || digit > '9')
        {
            SecureZeroMemory(pin.data(), pin.size());
            pin.clear();
            return false;
        }
        pin.push_back(digit);
    }
    return true;
}

bool StreamFrames(SchannelTlsStream& tls, ID3D11Device* device, ID3D11DeviceContext* context,
                  IDXGIOutput1* output, ComPtr<IDXGIOutputDuplication>& duplication,
                  mydisplay::video::H264Encoder& encoder)
{
    LARGE_INTEGER clockFrequency{};
    if (!QueryPerformanceFrequency(&clockFrequency))
    {
        std::wcerr << L"Could not query the high-resolution capture clock.\n";
        return false;
    }
    const LONGLONG frameIntervalTicks = clockFrequency.QuadPart / kTargetFramesPerSecond;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<std::uint8_t> outputPixels;
    std::vector<mydisplay::video::EncodedFrame> encodedFrames;
    std::uint64_t frameId = 0;
    ULONGLONG lastSent = 0;
    const auto sendEncodedFrames = [&]()
    {
        for (const auto& encodedFrame : encodedFrames)
        {
            if (!SendEncodedFrame(tls, encodedFrame))
            {
                std::wcerr << L"TLS H.264 frame send failed.\n";
                return false;
            }
        }
        encodedFrames.clear();
        return true;
    };

    for (;;)
    {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        HRESULT result = duplication->AcquireNextFrame(16, &frameInfo, &resource);
        if (result == DXGI_ERROR_WAIT_TIMEOUT)
        {
            if (!encoder.Poll(encodedFrames) || !sendEncodedFrames())
            {
                std::wcerr << L"H.264 encoder polling failed.\n";
                return false;
            }
            continue;
        }
        if (result == DXGI_ERROR_ACCESS_LOST)
        {
            staging.Reset();
            if (!RecreateDuplication(output, device, duplication))
            {
                return false;
            }
            continue;
        }
        if (FAILED(result))
        {
            std::wcerr << L"AcquireNextFrame failed: 0x" << std::hex << result << L'\n';
            return false;
        }

        ComPtr<ID3D11Texture2D> source;
        result = resource.As(&source);
        if (FAILED(result))
        {
            duplication->ReleaseFrame();
            std::wcerr << L"Frame resource is not a D3D11 texture: 0x" << std::hex << result << L'\n';
            return false;
        }

        D3D11_TEXTURE2D_DESC sourceDescription{};
        source->GetDesc(&sourceDescription);
        D3D11_TEXTURE2D_DESC stagingDescription{};
        if (staging)
        {
            staging->GetDesc(&stagingDescription);
        }
        if (!staging || stagingDescription.Width != sourceDescription.Width ||
            stagingDescription.Height != sourceDescription.Height || stagingDescription.Format != sourceDescription.Format)
        {
            if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                duplication->ReleaseFrame();
                std::wcerr << L"Unsupported source format: " << sourceDescription.Format << L'\n';
                return false;
            }
            D3D11_TEXTURE2D_DESC newStagingDescription = sourceDescription;
            newStagingDescription.Usage = D3D11_USAGE_STAGING;
            newStagingDescription.BindFlags = 0;
            newStagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            newStagingDescription.MiscFlags = 0;
            result = device->CreateTexture2D(&newStagingDescription, nullptr, &staging);
            if (FAILED(result))
            {
                duplication->ReleaseFrame();
                std::wcerr << L"Could not create staging texture: 0x" << std::hex << result << L'\n';
                return false;
            }
        }

        bool shouldSend = false;
        LARGE_INTEGER currentTime{};
        QueryPerformanceCounter(&currentTime);
        if (lastSent == 0 || currentTime.QuadPart - static_cast<LONGLONG>(lastSent) >= frameIntervalTicks)
        {
            if (!DownscaleFrame(context, source.Get(), staging.Get(), outputPixels))
            {
                duplication->ReleaseFrame();
                return false;
            }
            shouldSend = true;
        }

        const HRESULT releaseResult = duplication->ReleaseFrame();
        if (releaseResult == DXGI_ERROR_ACCESS_LOST)
        {
            staging.Reset();
            if (!RecreateDuplication(output, device, duplication))
            {
                return false;
            }
            continue;
        }
        if (FAILED(releaseResult))
        {
            std::wcerr << L"ReleaseFrame failed: 0x" << std::hex << releaseResult << L'\n';
            return false;
        }
        if (shouldSend)
        {
            if (!encoder.EncodeBgra(outputPixels.data(), outputPixels.size(), ++frameId, encodedFrames))
            {
                std::wcerr << L"H.264 encoding failed.\n";
                return false;
            }
            if (!sendEncodedFrames()) return false;
            lastSent = static_cast<ULONGLONG>(currentTime.QuadPart);
        }
    }
}

#ifdef _DEBUG
void ListH264Encoders()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
    {
        std::wcerr << L"Could not initialize COM for MFT enumeration: 0x" << std::hex << comResult << L'\n';
        return;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
    {
        if (SUCCEEDED(comResult)) CoUninitialize();
        std::wcerr << L"Could not start Media Foundation.\n";
        return;
    }

    MFT_REGISTER_TYPE_INFO inputType{ MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outputType{ MFMediaType_Video, MFVideoFormat_H264 };
    const std::array<std::pair<const wchar_t*, UINT32>, 2> queries{{
        { L"Synchronous", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER },
        { L"Hardware asynchronous", MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER }
    }};
    for (const auto& query : queries)
    {
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        const HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, query.second,
                                         &inputType, &outputType, &activations, &count);
        std::wcout << query.first << L" H.264 encoders: " << count << L'\n';
        if (FAILED(result))
        {
            std::wcerr << L"MFTEnumEx failed: 0x" << std::hex << result << L'\n';
        }
        for (UINT32 index = 0; index < count; ++index)
        {
            wchar_t* name = nullptr;
            UINT32 nameLength = 0;
            if (SUCCEEDED(activations[index]->GetAllocatedString(
                    MFT_FRIENDLY_NAME_Attribute, &name, &nameLength)))
            {
                std::wcout << L"  " << name << L'\n';
                CoTaskMemFree(name);
            }
            activations[index]->Release();
        }
        CoTaskMemFree(activations);
    }

    MFShutdown();
    if (SUCCEEDED(comResult)) CoUninitialize();
}

bool RunH264CodecSmokeTest()
{
    constexpr std::uint32_t width = 1920;
    constexpr std::uint32_t height = 1080;
    constexpr std::uint32_t framesPerSecond = 60;
    constexpr std::uint64_t smokeFrameCount = 60;
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            bgra[offset] = static_cast<std::uint8_t>(x & 0xFF);
            bgra[offset + 1] = static_cast<std::uint8_t>(y & 0xFF);
            bgra[offset + 2] = static_cast<std::uint8_t>((x + y) & 0xFF);
            bgra[offset + 3] = 255;
        }
    }

    std::vector<std::uint8_t> nv12;
    const ULONGLONG conversionStart = GetTickCount64();
    for (std::uint64_t frameId = 0; frameId < smokeFrameCount; ++frameId)
    {
        if (!mydisplay::video::ConvertBgraToNv12(bgra.data(), bgra.size(), width, height, nv12))
        {
            std::wcerr << L"H.264 smoke test: BGRA-to-NV12 conversion failed.\n";
            return false;
        }
    }
    const ULONGLONG conversionElapsed = GetTickCount64() - conversionStart;

    mydisplay::video::H264Encoder encoder;
    std::wcerr << L"H.264 smoke test: initializing the preferred encoder.\n";
    if (!encoder.Initialize(width, height, framesPerSecond, kTargetBitrate))
    {
        std::wcerr << L"H.264 smoke test: Media Foundation encoder initialization failed.\n";
        return false;
    }
    std::wcerr << L"H.264 smoke test: encoder initialized, hardware="
               << (encoder.IsHardwareEncoder() ? L"yes" : L"no") << L".\n";
    std::vector<mydisplay::video::EncodedFrame> encodedFrames;
    std::size_t liveEncodedFrameCount = 0;
    const ULONGLONG encodingStart = GetTickCount64();
    for (std::uint64_t frameId = 0; frameId < smokeFrameCount; ++frameId)
    {
        std::vector<mydisplay::video::EncodedFrame> outputFrames;
        if (!encoder.EncodeBgra(bgra.data(), bgra.size(), frameId, outputFrames))
        {
            std::wcerr << L"H.264 smoke test: encoding failed.\n";
            return false;
        }
        if (frameId == 0 || frameId == smokeFrameCount - 1)
        {
            std::wcerr << L"H.264 smoke test: accepted input frame " << frameId << L".\n";
        }
        liveEncodedFrameCount += outputFrames.size();
        encodedFrames.insert(encodedFrames.end(),
                             std::make_move_iterator(outputFrames.begin()),
                             std::make_move_iterator(outputFrames.end()));
    }
    const ULONGLONG encodingElapsed = GetTickCount64() - encodingStart;
    std::wcerr << L"H.264 smoke test: live frames emitted=" << liveEncodedFrameCount << L'\n';
    if (encodedFrames.empty())
    {
        std::wcerr << L"H.264 smoke test: encoder produced no frames.\n";
        return false;
    }
    mydisplay::video::H264Decoder decoder;
    if (!decoder.Initialize(width, height, framesPerSecond))
    {
        std::wcerr << L"H.264 smoke test: Media Foundation decoder initialization failed.\n";
        return false;
    }
    std::wcerr << L"H.264 smoke test: decoder hardware="
               << (decoder.IsHardwareDecoder() ? L"yes" : L"no") << L".\n";
    std::size_t decodedFrameCount = 0;
    const ULONGLONG decodingStart = GetTickCount64();
    for (const auto& encodedFrame : encodedFrames)
    {
        std::vector<mydisplay::video::DecodedFrame> decodedFrames;
        if (!decoder.Decode(encodedFrame.bytes.data(), encodedFrame.bytes.size(),
                            encodedFrame.frameId, decodedFrames))
        {
            std::wcerr << L"H.264 smoke test: decoding failed.\n";
            return false;
        }
        decodedFrameCount += decodedFrames.size();
    }
    const ULONGLONG decodingElapsed = GetTickCount64() - decodingStart;
    std::vector<mydisplay::video::DecodedFrame> drainedDecodedFrames;
    const bool decoderDrained = decoder.Flush(drainedDecodedFrames);
    decodedFrameCount += drainedDecodedFrames.size();
    const bool hasAnnexBStartCode = !encodedFrames.empty() && encodedFrames.front().bytes.size() >= 4 &&
        encodedFrames.front().bytes[0] == 0 && encodedFrames.front().bytes[1] == 0 &&
        encodedFrames.front().bytes[2] == 0 && encodedFrames.front().bytes[3] == 1;
    std::wcout << L"H.264 smoke probe: " << encodedFrames.size() << L" encoded ("
               << liveEncodedFrameCount << L" before encoder drain), " << decodedFrameCount
               << L" decoded before stream end, Annex B start code: "
               << (hasAnnexBStartCode ? L"yes" : L"no") << L", decoder drain: "
               << (decoderDrained ? L"ok" : L"failed") << L".\n";
    if (decodedFrameCount == 0)
    {
        std::wcerr << L"H.264 smoke test: decoder produced no frames.\n";
        return false;
    }

    std::size_t encodedBytes = 0;
    for (const auto& frame : encodedFrames)
    {
        encodedBytes += frame.bytes.size();
    }
    std::wcout << L"H.264 smoke test passed: " << encodedFrames.size() << L" encoded, "
               << decodedFrameCount << L" decoded 1080p frames; " << liveEncodedFrameCount
               << L" were available before encoder drain; " << encodedBytes
               << L" bytes from " << smokeFrameCount << L" synthetic frames; encode "
               << encodingElapsed << L" ms, decode " << decodingElapsed << L" ms, conversion "
               << conversionElapsed << L" ms.\n";
    return true;
}
#endif
}

int wmain(int argc, wchar_t* argv[])
{
#ifdef _DEBUG
    if (argc == 2 && _wcsicmp(argv[1], L"--list-h264-encoders") == 0)
    {
        ListH264Encoders();
        return 0;
    }
    if (argc == 2 && _wcsicmp(argv[1], L"--codec-smoke-test") == 0)
    {
        return RunH264CodecSmokeTest() ? 0 : 1;
    }
#endif
    const wchar_t* bindAddressText = argc > 1 ? argv[1] : L"127.0.0.1";
    const unsigned long portValue = argc > 2 ? std::wcstoul(argv[2], nullptr, 10) : 48001;
    const bool readSessionPinFromStdin = argc == 4 && _wcsicmp(argv[3], L"--pin-stdin") == 0;
    if (portValue == 0 || portValue > 65535)
    {
        std::wcerr << L"Port must be between 1 and 65535.\n";
        return 2;
    }

    PCCERT_CONTEXT certificate = FindServerCertificate();
    if (!certificate)
    {
        std::wcerr << L"Server TLS certificate not found. Run Installer\\New-ServerCertificate.ps1.\n";
        return 1;
    }
    const std::wstring fingerprint = Sha256Fingerprint(certificate);
    std::string sessionPin = CreateSessionPin();
    if (readSessionPinFromStdin)
    {
        SecureZeroMemory(sessionPin.data(), sessionPin.size());
        sessionPin.clear();
        if (!ReadSessionPinFromStdin(sessionPin))
        {
            std::wcerr << L"Could not receive the pairing PIN from the setup window.\n";
            CertFreeCertificateContext(certificate);
            return 1;
        }
    }
#ifdef _DEBUG
    if (argc == 5 && _wcsicmp(bindAddressText, L"127.0.0.1") == 0 &&
        _wcsicmp(argv[3], L"--test-pin") == 0)
    {
        const std::wstring testPin = argv[4];
        if (testPin.size() == mydisplay::protocol::kPairingPinLength &&
            std::all_of(testPin.begin(), testPin.end(), [](wchar_t digit) { return digit >= L'0' && digit <= L'9'; }))
        {
            sessionPin.clear();
            for (const wchar_t digit : testPin)
            {
                sessionPin.push_back(static_cast<char>(digit));
            }
        }
        else
        {
            std::wcerr << L"Debug loopback test PIN must contain six digits.\n";
            CertFreeCertificateContext(certificate);
            return 2;
        }
    }
#else
    if ((argc > 3 && !readSessionPinFromStdin) || argc > 4)
    {
        std::wcerr << L"Unexpected command-line arguments.\n";
        CertFreeCertificateContext(certificate);
        return 2;
    }
#endif
    if (fingerprint.empty() || sessionPin.empty())
    {
        std::wcerr << L"Could not prepare the TLS fingerprint or pairing PIN.\n";
        CertFreeCertificateContext(certificate);
        return 1;
    }

    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0)
    {
        CertFreeCertificateContext(certificate);
        return 1;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in bindAddress{};
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = htons(static_cast<u_short>(portValue));
    if (listener == INVALID_SOCKET || InetPtonW(AF_INET, bindAddressText, &bindAddress.sin_addr) != 1 ||
        bind(listener, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) == SOCKET_ERROR ||
        listen(listener, 2) == SOCKET_ERROR)
    {
        std::wcerr << L"Could not bind TLS listener at " << bindAddressText << L':' << portValue
                   << L" (Winsock " << WSAGetLastError() << L").\n";
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        CertFreeCertificateContext(certificate);
        return 1;
    }

    std::wcout << L"TLS listener: " << bindAddressText << L':' << portValue << L'\n';
    std::wcout << L"Server certificate SHA-256: " << fingerprint << L'\n';
    std::wcout << L"Waiting for an authenticated client...\n";
    if (!readSessionPinFromStdin)
    {
        std::wcout << L"One-session pairing PIN: " << std::wstring(sessionPin.begin(), sessionPin.end()) << L'\n';
    }

    SOCKET clientSocket = accept(listener, nullptr, nullptr);
    bool authorized = false;
    if (clientSocket != INVALID_SOCKET)
    {
        const BOOL noDelay = TRUE;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        SchannelTlsStream tls;
        if (tls.Accept(clientSocket, certificate))
        {
            mydisplay::protocol::PairingRequest request{};
            if (tls.ReceiveExact(&request, sizeof(request)))
            {
                const bool validPin = VerifyPin(request, sessionPin);
                mydisplay::protocol::PairingResponse response{};
                response.magic = validPin ? mydisplay::protocol::kPairingAcceptedMagic : mydisplay::protocol::kPairingRejectedMagic;
                response.version = mydisplay::protocol::kSecureProtocolVersion;
                response.status = validPin ? 0 : 1;
                tls.SendAll(&response, sizeof(response));
                authorized = validPin;
            }
        }

        if (!authorized)
        {
            std::wcerr << L"TLS client authentication failed; no video was sent.\n";
        }
        else
        {
            std::wstring displayName;
            std::wstring friendlyName;
            if (!FindSampleDisplay(displayName, friendlyName))
            {
                std::wcerr << L"Virtual monitor with PnP ID DELD0E6 is not active.\n";
            }
            else
            {
                std::wcout << L"Authorized client connected. Streaming " << friendlyName
                           << L" at 1920x1080, up to 60 FPS, target 50 Mbps H.264.\n";
                ComPtr<IDXGIFactory1> factory;
                ComPtr<IDXGIAdapter1> adapterMatch;
                ComPtr<IDXGIOutput> outputMatch;
                HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
                UINT matches = 0;
                for (UINT adapterIndex = 0; SUCCEEDED(result); ++adapterIndex)
                {
                    ComPtr<IDXGIAdapter1> adapter;
                    result = factory->EnumAdapters1(adapterIndex, &adapter);
                    if (result == DXGI_ERROR_NOT_FOUND) { result = S_OK; break; }
                    if (FAILED(result)) break;
                    for (UINT outputIndex = 0;; ++outputIndex)
                    {
                        ComPtr<IDXGIOutput> output;
                        result = adapter->EnumOutputs(outputIndex, &output);
                        if (result == DXGI_ERROR_NOT_FOUND) { result = S_OK; break; }
                        if (FAILED(result)) break;
                        DXGI_OUTPUT_DESC description{};
                        if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop &&
                            _wcsicmp(description.DeviceName, displayName.c_str()) == 0)
                        {
                            ++matches;
                            adapterMatch = adapter;
                            outputMatch = output;
                        }
                    }
                }

                ComPtr<ID3D11Device> device;
                ComPtr<ID3D11DeviceContext> context;
                ComPtr<IDXGIOutput1> output1;
                ComPtr<IDXGIOutputDuplication> duplication;
                if (SUCCEEDED(result) && matches == 1)
                {
                    result = D3D11CreateDevice(adapterMatch.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                               D3D11_SDK_VERSION, &device, nullptr, &context);
                }
                else
                {
                    result = E_FAIL;
                }
                if (SUCCEEDED(result)) result = outputMatch.As(&output1);
                if (SUCCEEDED(result)) result = output1->DuplicateOutput(device.Get(), &duplication);
                if (FAILED(result))
                {
                    std::wcerr << L"Could not initialize DXGI duplication for the verified virtual monitor: 0x"
                               << std::hex << result << L'\n';
                }
                else
                {
                    mydisplay::video::H264Encoder encoder;
                    if (!encoder.Initialize(kOutputWidth, kOutputHeight,
                                            kTargetFramesPerSecond, kTargetBitrate))
                    {
                        std::wcerr << L"Could not initialize the Media Foundation H.264 encoder.\n";
                    }
                    else
                    {
                        StreamFrames(tls, device.Get(), context.Get(), output1.Get(), duplication, encoder);
                    }
                }
            }
        }
        closesocket(clientSocket);
    }

    closesocket(listener);
    WSACleanup();
    CertFreeCertificateContext(certificate);
    return authorized ? 0 : 1;
}