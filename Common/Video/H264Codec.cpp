#define NOMINMAX
#include "H264Codec.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <codecapi.h>
#include <strmif.h>
#include <ppl.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfuuid.lib")
#pragma comment(lib, "Strmiids.lib")

namespace
{
constexpr std::uint64_t kHundredNanosecondsPerSecond = 10000000;
constexpr std::size_t kMaxPendingEncoderFrames = 3;

bool InitializeMediaFoundation(bool& comInitialized, bool& mediaFoundationStarted)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult))
    {
        comInitialized = true;
    }
    else if (comResult != RPC_E_CHANGED_MODE)
    {
        return false;
    }

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
    {
        if (comInitialized)
        {
            CoUninitialize();
            comInitialized = false;
        }
        return false;
    }
    mediaFoundationStarted = true;
    return true;
}

void ShutdownMediaFoundation(bool& comInitialized, bool& mediaFoundationStarted)
{
    if (mediaFoundationStarted)
    {
        MFShutdown();
        mediaFoundationStarted = false;
    }
    if (comInitialized)
    {
        CoUninitialize();
        comInitialized = false;
    }
}

bool CreateMediaType(IMFMediaType** mediaType, const GUID& subtype,
                     std::uint32_t width, std::uint32_t height,
                     std::uint32_t framesPerSecond)
{
    if (FAILED(MFCreateMediaType(mediaType)))
    {
        return false;
    }
    return SUCCEEDED((*mediaType)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
           SUCCEEDED((*mediaType)->SetGUID(MF_MT_SUBTYPE, subtype)) &&
           SUCCEEDED(MFSetAttributeSize(*mediaType, MF_MT_FRAME_SIZE, width, height)) &&
           SUCCEEDED(MFSetAttributeRatio(*mediaType, MF_MT_FRAME_RATE, framesPerSecond, 1)) &&
           SUCCEEDED(MFSetAttributeRatio(*mediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
}

void SetCodecUInt32(ICodecAPI* codec, const GUID& property, ULONG value)
{
    if (!codec)
    {
        return;
    }
    VARIANT setting{};
    setting.vt = VT_UI4;
    setting.ulVal = value;
    codec->SetValue(&property, &setting);
    VariantClear(&setting);
}

void SetCodecBool(ICodecAPI* codec, const GUID& property, VARIANT_BOOL value)
{
    if (!codec)
    {
        return;
    }
    VARIANT setting{};
    setting.vt = VT_BOOL;
    setting.boolVal = value;
    codec->SetValue(&property, &setting);
    VariantClear(&setting);
}

bool ActivateTransform(const GUID& category, const GUID& inputSubtype,
                       const GUID& outputSubtype, UINT32 flags,
                       IMFTransform** transform)
{
    MFT_REGISTER_TYPE_INFO inputType{ MFMediaType_Video, inputSubtype };
    MFT_REGISTER_TYPE_INFO outputType{ MFMediaType_Video, outputSubtype };
    IMFActivate** activations = nullptr;
    UINT32 activationCount = 0;
    const HRESULT enumerateResult = MFTEnumEx(
        category, flags,
        &inputType, &outputType, &activations, &activationCount);
    if (FAILED(enumerateResult) || activationCount == 0)
    {
        CoTaskMemFree(activations);
        return false;
    }

    const HRESULT activateResult = activations[0]->ActivateObject(IID_PPV_ARGS(transform));
    for (UINT32 index = 0; index < activationCount; ++index)
    {
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    return SUCCEEDED(activateResult);
}

bool CreateInputSample(const std::uint8_t* bytes, std::size_t byteCount,
                       std::uint64_t frameId, std::uint32_t framesPerSecond,
                       IMFSample** sample)
{
    if (byteCount > std::numeric_limits<DWORD>::max())
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(byteCount), &buffer)))
    {
        return false;
    }
    BYTE* destination = nullptr;
    DWORD maximumLength = 0;
    if (FAILED(buffer->Lock(&destination, &maximumLength, nullptr)) || maximumLength < byteCount)
    {
        if (destination)
        {
            buffer->Unlock();
        }
        return false;
    }
    std::memcpy(destination, bytes, byteCount);
    buffer->Unlock();
    if (FAILED(buffer->SetCurrentLength(static_cast<DWORD>(byteCount))) ||
        FAILED(MFCreateSample(sample)) || FAILED((*sample)->AddBuffer(buffer.Get())))
    {
        if (*sample)
        {
            (*sample)->Release();
            *sample = nullptr;
        }
        return false;
    }

    const LONGLONG frameDuration = static_cast<LONGLONG>(kHundredNanosecondsPerSecond / framesPerSecond);
    const LONGLONG sampleTime = static_cast<LONGLONG>(
        frameId * kHundredNanosecondsPerSecond / framesPerSecond);
    return SUCCEEDED((*sample)->SetSampleTime(sampleTime)) &&
           SUCCEEDED((*sample)->SetSampleDuration(frameDuration));
}

std::uint64_t FrameIdFromSample(IMFSample* sample, std::uint32_t framesPerSecond,
                                std::uint64_t fallbackFrameId)
{
    LONGLONG sampleTime = 0;
    if (SUCCEEDED(sample->GetSampleTime(&sampleTime)) && sampleTime >= 0)
    {
        return (static_cast<std::uint64_t>(sampleTime) * framesPerSecond +
            kHundredNanosecondsPerSecond / 2) / kHundredNanosecondsPerSecond;
    }
    return fallbackFrameId;
}

bool CreateOutputSample(DWORD bufferSize, MFT_OUTPUT_DATA_BUFFER& outputData)
{
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    HRESULT result = MFCreateMemoryBuffer(bufferSize, &buffer);
    if (SUCCEEDED(result))
    {
        result = MFCreateSample(&sample);
    }
    if (SUCCEEDED(result))
    {
        result = sample->AddBuffer(buffer);
    }
    if (buffer)
    {
        buffer->Release();
    }
    if (FAILED(result))
    {
        if (sample)
        {
            sample->Release();
        }
        return false;
    }
    outputData.pSample = sample;
    return true;
}

void ReleaseOutputData(MFT_OUTPUT_DATA_BUFFER& outputData)
{
    if (outputData.pSample)
    {
        outputData.pSample->Release();
        outputData.pSample = nullptr;
    }
    if (outputData.pEvents)
    {
        outputData.pEvents->Release();
        outputData.pEvents = nullptr;
    }
}

bool CopySampleBytes(IMFSample* sample, std::vector<std::uint8_t>& bytes)
{
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
    {
        return false;
    }
    BYTE* data = nullptr;
    DWORD maximumLength = 0;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&data, &maximumLength, &currentLength)))
    {
        return false;
    }
    if (currentLength > 0 && currentLength <= maximumLength)
    {
        bytes.assign(data, data + currentLength);
    }
    buffer->Unlock();
    return currentLength > 0 && currentLength <= maximumLength;
}

bool AppendEncodedSample(IMFSample* sample, std::uint32_t framesPerSecond,
                        std::uint64_t fallbackFrameId, std::vector<mydisplay::video::EncodedFrame>& output)
{
    mydisplay::video::EncodedFrame frame;
    frame.frameId = FrameIdFromSample(sample, framesPerSecond, fallbackFrameId);
    if (!CopySampleBytes(sample, frame.bytes))
    {
        return false;
    }
    output.push_back(std::move(frame));
    return true;
}

bool AppendDecodedSample(IMFSample* sample, std::uint32_t width, std::uint32_t height,
                         std::uint32_t framesPerSecond, std::uint64_t fallbackFrameId,
                         std::vector<mydisplay::video::DecodedFrame>& output)
{
    std::vector<std::uint8_t> nv12;
    if (!CopySampleBytes(sample, nv12))
    {
        return false;
    }
    mydisplay::video::DecodedFrame frame;
    frame.frameId = FrameIdFromSample(sample, framesPerSecond, fallbackFrameId);
    if (!mydisplay::video::ConvertNv12ToBgra(nv12.data(), nv12.size(), width, height, frame.bgra))
    {
        return false;
    }
    output.push_back(std::move(frame));
    return true;
}

int ClampByte(int value)
{
    return (std::max)(0, (std::min)(255, value));
}
}

namespace mydisplay::video
{
H264Encoder::~H264Encoder()
{
    transform_.Reset();
    ShutdownMediaFoundation(comInitialized_, mediaFoundationStarted_);
}

bool H264Encoder::Initialize(std::uint32_t width, std::uint32_t height,
                             std::uint32_t framesPerSecond, std::uint32_t bitrate)
{
    if ((width & 1) != 0 || (height & 1) != 0 || framesPerSecond == 0 || bitrate == 0 ||
        !InitializeMediaFoundation(comInitialized_, mediaFoundationStarted_))
    {
        return false;
    }

    const UINT32 hardwareFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                                 MFT_ENUM_FLAG_SORTANDFILTER;
    if (ActivateTransform(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12,
                          MFVideoFormat_H264, hardwareFlags, &transform_))
    {
        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        if (FAILED(transform_->GetAttributes(&attributes)) ||
            FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)) ||
            FAILED(transform_.As(&eventGenerator_)))
        {
            transform_.Reset();
            eventGenerator_.Reset();
        }
        else
        {
            asynchronous_ = true;
        }
    }
    if (!transform_ && !ActivateTransform(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12,
                                          MFVideoFormat_H264,
                                          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                          &transform_))
    {
        return false;
    }

    width_ = width;
    height_ = height;
    framesPerSecond_ = framesPerSecond;
    bitrate_ = bitrate;

    Microsoft::WRL::ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform_.As(&codec)))
    {
        SetCodecUInt32(codec.Get(), CODECAPI_AVEncCommonRateControlMode,
                       eAVEncCommonRateControlMode_CBR);
        SetCodecUInt32(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
        SetCodecUInt32(codec.Get(), CODECAPI_AVEncCommonBufferSize,
                       bitrate / framesPerSecond / 4);
        SetCodecUInt32(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetCodecUInt32(codec.Get(), CODECAPI_AVEncMPVGOPSize, framesPerSecond * 2);
        SetCodecBool(codec.Get(), CODECAPI_AVEncCommonLowLatency, VARIANT_TRUE);
    }

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    if (!CreateMediaType(&outputType, MFVideoFormat_H264, width, height, framesPerSecond) ||
        FAILED(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate)) ||
        FAILED(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main)) ||
        FAILED(transform_->SetOutputType(0, outputType.Get(), 0)))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    if (!CreateMediaType(&inputType, MFVideoFormat_NV12, width, height, framesPerSecond) ||
        FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        FAILED(inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width)) ||
        FAILED(transform_->SetInputType(0, inputType.Get(), 0)) ||
        FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
    {
        return false;
    }
    return true;
}

bool H264Encoder::SetOutputType()
{
    for (DWORD index = 0;; ++index)
    {
        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        const HRESULT result = transform_->GetOutputAvailableType(0, index, &outputType);
        if (result == MF_E_NO_MORE_TYPES)
        {
            Microsoft::WRL::ComPtr<IMFMediaType> currentType;
            GUID currentSubtype{};
            return SUCCEEDED(transform_->GetOutputCurrentType(0, &currentType)) &&
                   SUCCEEDED(currentType->GetGUID(MF_MT_SUBTYPE, &currentSubtype)) &&
                   currentSubtype == MFVideoFormat_H264;
        }
        if (FAILED(result))
        {
            return false;
        }

        GUID subtype{};
        if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == MFVideoFormat_H264 &&
            SUCCEEDED(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width_, height_)) &&
            SUCCEEDED(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, framesPerSecond_, 1)) &&
            SUCCEEDED(outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate_)) &&
            SUCCEEDED(transform_->SetOutputType(0, outputType.Get(), 0)))
        {
            return true;
        }
    }
}

bool H264Encoder::EncodeBgra(const std::uint8_t* bgra, std::size_t bgraBytes,
                             std::uint64_t frameId, std::vector<EncodedFrame>& output)
{
    std::vector<std::uint8_t> nv12;
    if (!ConvertBgraToNv12(bgra, bgraBytes, width_, height_, nv12))
    {
        return false;
    }
    currentFrameId_ = frameId;
    if (asynchronous_)
    {
        if (pendingInputs_.size() >= kMaxPendingEncoderFrames)
        {
            pendingInputs_.pop_front();
        }
        pendingInputs_.push_back({ frameId, std::move(nv12) });
        return PumpAsync(output, false);
    }
    if (!DrainOutput(output))
    {
        return false;
    }

    IMFSample* sample = nullptr;
    if (!CreateInputSample(nv12.data(), nv12.size(), frameId, framesPerSecond_, &sample))
    {
        return false;
    }
    HRESULT result = transform_->ProcessInput(0, sample, 0);
    sample->Release();
    if (result == MF_E_NOTACCEPTING)
    {
        if (!DrainOutput(output))
        {
            return false;
        }
        IMFSample* retrySample = nullptr;
        if (!CreateInputSample(nv12.data(), nv12.size(), frameId, framesPerSecond_, &retrySample))
        {
            return false;
        }
        result = transform_->ProcessInput(0, retrySample, 0);
        retrySample->Release();
    }
    return SUCCEEDED(result) && DrainOutput(output);
}

bool H264Encoder::Flush(std::vector<EncodedFrame>& output)
{
    if (asynchronous_)
    {
        return PumpAsync(output, true);
    }
    return SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)) &&
           SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)) &&
           DrainOutput(output);
}

bool H264Encoder::Poll(std::vector<EncodedFrame>& output)
{
    return asynchronous_ ? PumpAsync(output, false) : DrainOutput(output);
}

bool H264Encoder::PumpAsync(std::vector<EncodedFrame>& output, bool drainToEnd)
{
    if (!eventGenerator_)
    {
        return false;
    }
    if (drainToEnd &&
        (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)) ||
         FAILED(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))))
    {
        return false;
    }

    bool drainComplete = false;
    for (;;)
    {
        bool madeProgress = false;
        for (;;)
        {
            Microsoft::WRL::ComPtr<IMFMediaEvent> event;
            const HRESULT eventResult = eventGenerator_->GetEvent(
                drainToEnd ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);
            if (eventResult == MF_E_NO_EVENTS_AVAILABLE)
            {
                break;
            }
            if (FAILED(eventResult))
            {
                return false;
            }

            MediaEventType eventType{};
            if (FAILED(event->GetType(&eventType)))
            {
                return false;
            }
            switch (eventType)
            {
            case METransformNeedInput:
                ++needInputEvents_;
                break;
            case METransformHaveOutput:
                ++haveOutputEvents_;
                break;
            case METransformDrainComplete:
                drainComplete = true;
                break;
            case MEError:
            {
                HRESULT status = E_FAIL;
                event->GetStatus(&status);
                std::wcerr << L"Hardware H.264 encoder event failed: 0x" << std::hex << status << L'\n';
                return false;
            }
            default:
                break;
            }
            madeProgress = true;
        }

        while (haveOutputEvents_ > 0)
        {
            if (!ProcessAsyncOutput(output))
            {
                return false;
            }
            --haveOutputEvents_;
            madeProgress = true;
        }

        while (needInputEvents_ > 0 && !pendingInputs_.empty())
        {
            PendingInput& pending = pendingInputs_.front();
            IMFSample* sample = nullptr;
            if (!CreateInputSample(pending.nv12.data(), pending.nv12.size(),
                                   pending.frameId, framesPerSecond_, &sample))
            {
                return false;
            }
            const HRESULT inputResult = transform_->ProcessInput(0, sample, 0);
            sample->Release();
            if (inputResult == MF_E_NOTACCEPTING)
            {
                needInputEvents_ = 0;
                break;
            }
            if (FAILED(inputResult))
            {
                std::wcerr << L"Hardware H.264 encoder rejected an NV12 frame: 0x"
                           << std::hex << inputResult << L'\n';
                return false;
            }
            pendingInputs_.pop_front();
            --needInputEvents_;
            madeProgress = true;
        }

        if (drainComplete)
        {
            return pendingInputs_.empty();
        }
        if (!drainToEnd && !madeProgress)
        {
            return true;
        }
    }
}

bool H264Encoder::ProcessAsyncOutput(std::vector<EncodedFrame>& output)
{
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (FAILED(transform_->GetOutputStreamInfo(0, &streamInfo)))
    {
        return false;
    }
    MFT_OUTPUT_DATA_BUFFER outputData{};
    outputData.dwStreamID = 0;
    const DWORD outputBufferSize = (std::max)(streamInfo.cbSize, static_cast<DWORD>(1024 * 1024));
    if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0 &&
        !CreateOutputSample(outputBufferSize, outputData))
    {
        return false;
    }
    DWORD processStatus = 0;
    const HRESULT result = transform_->ProcessOutput(0, 1, &outputData, &processStatus);
    if (result == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        ReleaseOutputData(outputData);
        if (!SetOutputType())
        {
            std::wcerr << L"Hardware H.264 encoder could not negotiate its output type.\n";
            return false;
        }
        return true;
    }
    if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        ReleaseOutputData(outputData);
        return true;
    }
    if (FAILED(result))
    {
        std::wcerr << L"Hardware H.264 ProcessOutput failed: 0x" << std::hex << result << L'\n';
        ReleaseOutputData(outputData);
        return false;
    }
    const bool copied = !outputData.pSample ||
        AppendEncodedSample(outputData.pSample, framesPerSecond_, currentFrameId_, output);
    const bool moreOutput = (outputData.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) != 0;
    ReleaseOutputData(outputData);
    if (moreOutput)
    {
        ++haveOutputEvents_;
    }
    return copied;
}

bool H264Encoder::DrainOutput(std::vector<EncodedFrame>& output)
{
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (FAILED(transform_->GetOutputStreamInfo(0, &streamInfo)))
    {
        return false;
    }
    const DWORD outputBufferSize = (std::max)(streamInfo.cbSize, static_cast<DWORD>(1024 * 1024));
    for (;;)
    {
        MFT_OUTPUT_DATA_BUFFER outputData{};
        outputData.dwStreamID = 0;
        if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0 &&
            !CreateOutputSample(outputBufferSize, outputData))
        {
            return false;
        }
        DWORD processStatus = 0;
        const HRESULT result = transform_->ProcessOutput(0, 1, &outputData, &processStatus);
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            ReleaseOutputData(outputData);
            return true;
        }
        if (FAILED(result))
        {
            ReleaseOutputData(outputData);
            return false;
        }
        const bool copied = !outputData.pSample ||
            AppendEncodedSample(outputData.pSample, framesPerSecond_, currentFrameId_, output);
        ReleaseOutputData(outputData);
        if (!copied)
        {
            return false;
        }
    }
}

H264Decoder::~H264Decoder()
{
    eventGenerator_.Reset();
    transform_.Reset();
    ShutdownMediaFoundation(comInitialized_, mediaFoundationStarted_);
}

bool H264Decoder::Initialize(std::uint32_t width, std::uint32_t height,
                             std::uint32_t framesPerSecond)
{
    if ((width & 1) != 0 || (height & 1) != 0 || framesPerSecond == 0 ||
        !InitializeMediaFoundation(comInitialized_, mediaFoundationStarted_))
    {
        return false;
    }

    const UINT32 hardwareFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                                 MFT_ENUM_FLAG_SORTANDFILTER;
    if (ActivateTransform(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_H264,
                          MFVideoFormat_NV12, hardwareFlags, &transform_))
    {
        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        if (FAILED(transform_->GetAttributes(&attributes)) ||
            FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)) ||
            FAILED(transform_.As(&eventGenerator_)))
        {
            transform_.Reset();
            eventGenerator_.Reset();
        }
        else
        {
            asynchronous_ = true;
        }
    }
    if (!transform_ && !ActivateTransform(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_H264,
                                          MFVideoFormat_NV12,
                                          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                          &transform_))
    {
        return false;
    }

    width_ = width;
    height_ = height;
    framesPerSecond_ = framesPerSecond;
    const auto configureTransform = [&]()
    {
        Microsoft::WRL::ComPtr<IMFMediaType> inputType;
        return CreateMediaType(&inputType, MFVideoFormat_H264, width, height, framesPerSecond) &&
            SUCCEEDED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive)) &&
            SUCCEEDED(transform_->SetInputType(0, inputType.Get(), 0)) &&
            SetOutputType() &&
            SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) &&
            SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    };
    if (configureTransform())
    {
        return true;
    }
    if (!asynchronous_)
    {
        return false;
    }

    eventGenerator_.Reset();
    transform_.Reset();
    asynchronous_ = false;
    if (!ActivateTransform(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_H264,
                           MFVideoFormat_NV12,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           &transform_))
    {
        return false;
    }
    return configureTransform();
}

bool H264Decoder::SetOutputType()
{
    Microsoft::WRL::ComPtr<IMFMediaType> currentType;
    GUID currentSubtype{};
    if (SUCCEEDED(transform_->GetOutputCurrentType(0, &currentType)) &&
        SUCCEEDED(currentType->GetGUID(MF_MT_SUBTYPE, &currentSubtype)) &&
        currentSubtype == MFVideoFormat_NV12)
    {
        return true;
    }

    for (DWORD index = 0;; ++index)
    {
        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        const HRESULT result = transform_->GetOutputAvailableType(0, index, &outputType);
        if (result == MF_E_NO_MORE_TYPES)
        {
            std::wcerr << L"H.264 decoder exposed no usable NV12 output type.\n";
            return false;
        }
        if (FAILED(result))
        {
            std::wcerr << L"H.264 decoder GetOutputAvailableType failed: 0x"
                       << std::hex << result << L'\n';
            return false;
        }
        GUID subtype{};
        if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12)
        {
            if (SUCCEEDED(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width_, height_)) &&
                SUCCEEDED(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, framesPerSecond_, 1)) &&
                SUCCEEDED(outputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width_)) &&
                SUCCEEDED(transform_->SetOutputType(0, outputType.Get(), 0)))
            {
                return true;
            }
        }
    }
}

bool H264Decoder::Decode(const std::uint8_t* encoded, std::size_t encodedBytes,
                         std::uint64_t frameId, std::vector<DecodedFrame>& output)
{
    currentFrameId_ = frameId;
    if (asynchronous_)
    {
        PendingAccessUnit input;
        input.frameId = frameId;
        input.bytes.assign(encoded, encoded + encodedBytes);
        pendingInputs_.push_back(std::move(input));
        return PumpAsync(output);
    }
    if (!DrainOutput(output))
    {
        return false;
    }
    currentFrameId_ = frameId;
    IMFSample* sample = nullptr;
    if (!CreateInputSample(encoded, encodedBytes, frameId, framesPerSecond_, &sample))
    {
        return false;
    }
    HRESULT result = transform_->ProcessInput(0, sample, 0);
    sample->Release();
    if (result == MF_E_NOTACCEPTING)
    {
        if (!DrainOutput(output))
        {
            return false;
        }
        IMFSample* retrySample = nullptr;
        if (!CreateInputSample(encoded, encodedBytes, frameId, framesPerSecond_, &retrySample))
        {
            return false;
        }
        result = transform_->ProcessInput(0, retrySample, 0);
        retrySample->Release();
    }
    return SUCCEEDED(result) && DrainOutput(output);
}

bool H264Decoder::Flush(std::vector<DecodedFrame>& output)
{
    if (asynchronous_)
    {
        return PumpAsync(output, true);
    }
    const HRESULT endOfStreamResult = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(endOfStreamResult))
    {
        std::wcerr << L"H.264 decoder end-of-stream message failed: 0x"
                   << std::hex << endOfStreamResult << L'\n';
        return false;
    }
    const HRESULT drainResult = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(drainResult))
    {
        std::wcerr << L"H.264 decoder drain message failed: 0x"
                   << std::hex << drainResult << L'\n';
        return false;
    }
    return DrainOutput(output);
}

bool H264Decoder::PumpAsync(std::vector<DecodedFrame>& output, bool drainToEnd)
{
    if (!eventGenerator_)
    {
        return false;
    }
    if (drainToEnd &&
        (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)) ||
         FAILED(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))))
    {
        return false;
    }

    bool drainComplete = false;
    for (;;)
    {
        bool madeProgress = false;
        for (;;)
        {
            Microsoft::WRL::ComPtr<IMFMediaEvent> event;
            const HRESULT eventResult = eventGenerator_->GetEvent(
                drainToEnd ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);
            if (eventResult == MF_E_NO_EVENTS_AVAILABLE)
            {
                break;
            }
            if (FAILED(eventResult))
            {
                return false;
            }

            MediaEventType eventType{};
            if (FAILED(event->GetType(&eventType)))
            {
                return false;
            }
            switch (eventType)
            {
            case METransformNeedInput:
                ++needInputEvents_;
                break;
            case METransformHaveOutput:
                ++haveOutputEvents_;
                break;
            case METransformDrainComplete:
                drainComplete = true;
                break;
            case MEError:
            {
                HRESULT status = E_FAIL;
                event->GetStatus(&status);
                std::wcerr << L"Hardware H.264 decoder event failed: 0x" << std::hex << status << L'\n';
                return false;
            }
            default:
                break;
            }
            madeProgress = true;
        }

        while (haveOutputEvents_ > 0)
        {
            if (!ProcessAsyncOutput(output))
            {
                return false;
            }
            --haveOutputEvents_;
            madeProgress = true;
        }

        while (needInputEvents_ > 0 && !pendingInputs_.empty())
        {
            PendingAccessUnit& input = pendingInputs_.front();
            IMFSample* sample = nullptr;
            if (!CreateInputSample(input.bytes.data(), input.bytes.size(),
                                   input.frameId, framesPerSecond_, &sample))
            {
                return false;
            }
            const HRESULT inputResult = transform_->ProcessInput(0, sample, 0);
            sample->Release();
            if (inputResult == MF_E_NOTACCEPTING)
            {
                needInputEvents_ = 0;
                break;
            }
            if (FAILED(inputResult))
            {
                std::wcerr << L"Hardware H.264 decoder rejected an access unit: 0x"
                           << std::hex << inputResult << L'\n';
                return false;
            }
            pendingInputs_.pop_front();
            --needInputEvents_;
            madeProgress = true;
        }

        if (drainComplete)
        {
            return pendingInputs_.empty();
        }
        if (!drainToEnd && !madeProgress)
        {
            return true;
        }
    }
}

bool H264Decoder::ProcessAsyncOutput(std::vector<DecodedFrame>& output)
{
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (FAILED(transform_->GetOutputStreamInfo(0, &streamInfo)))
    {
        return false;
    }
    MFT_OUTPUT_DATA_BUFFER outputData{};
    outputData.dwStreamID = 0;
    const std::size_t requiredBytes = static_cast<std::size_t>(width_) * height_ * 2;
    const DWORD outputBufferSize = (std::max)(streamInfo.cbSize,
        static_cast<DWORD>((std::min)(requiredBytes, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))));
    if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0 &&
        !CreateOutputSample(outputBufferSize, outputData))
    {
        return false;
    }
    DWORD processStatus = 0;
    const HRESULT result = transform_->ProcessOutput(0, 1, &outputData, &processStatus);
    if (result == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        ReleaseOutputData(outputData);
        if (!SetOutputType())
        {
            return false;
        }
        return true;
    }
    if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        ReleaseOutputData(outputData);
        return true;
    }
    if (FAILED(result))
    {
        std::wcerr << L"Hardware H.264 decoder ProcessOutput failed: 0x" << std::hex << result << L'\n';
        ReleaseOutputData(outputData);
        return false;
    }
    const bool copied = !outputData.pSample ||
        AppendDecodedSample(outputData.pSample, width_, height_, framesPerSecond_, currentFrameId_, output);
    const bool moreOutput = (outputData.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) != 0;
    ReleaseOutputData(outputData);
    if (moreOutput)
    {
        ++haveOutputEvents_;
    }
    return copied;
}

bool H264Decoder::DrainOutput(std::vector<DecodedFrame>& output)
{
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (FAILED(transform_->GetOutputStreamInfo(0, &streamInfo)))
    {
        return false;
    }
    const std::size_t requiredBytes = static_cast<std::size_t>(width_) * height_ * 2;
    const DWORD outputBufferSize = (std::max)(streamInfo.cbSize,
        static_cast<DWORD>((std::min)(requiredBytes, static_cast<std::size_t>(std::numeric_limits<DWORD>::max()))));
    for (;;)
    {
        MFT_OUTPUT_DATA_BUFFER outputData{};
        outputData.dwStreamID = 0;
        if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0 &&
            !CreateOutputSample(outputBufferSize, outputData))
        {
            return false;
        }
        DWORD processStatus = 0;
        const HRESULT result = transform_->ProcessOutput(0, 1, &outputData, &processStatus);
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            ReleaseOutputData(outputData);
            return true;
        }
        if (result == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            ReleaseOutputData(outputData);
            if (!SetOutputType())
            {
                return false;
            }
            continue;
        }
        if (FAILED(result))
        {
            std::wcerr << L"H.264 decoder ProcessOutput failed: 0x"
                       << std::hex << result << L'\n';
            ReleaseOutputData(outputData);
            return false;
        }
        const bool copied = !outputData.pSample ||
            AppendDecodedSample(outputData.pSample, width_, height_, framesPerSecond_, currentFrameId_, output);
        ReleaseOutputData(outputData);
        if (!copied)
        {
            return false;
        }
    }
}

bool ConvertBgraToNv12(const std::uint8_t* bgra, std::size_t bgraBytes,
                       std::uint32_t width, std::uint32_t height,
                       std::vector<std::uint8_t>& nv12)
{
    if (!bgra || width == 0 || height == 0 || (width & 1) != 0 || (height & 1) != 0)
    {
        return false;
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    const std::size_t requiredBytes = pixelCount * 4;
    if (bgraBytes != requiredBytes || pixelCount > (std::numeric_limits<std::size_t>::max() / 3) * 2)
    {
        return false;
    }

    nv12.resize(pixelCount + pixelCount / 2);
    std::uint8_t* yPlane = nv12.data();
    std::uint8_t* uvPlane = yPlane + pixelCount;
    Concurrency::parallel_for(0u, height, [&](std::uint32_t y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint8_t* pixel = bgra + (static_cast<std::size_t>(y) * width + x) * 4;
            const int blue = pixel[0];
            const int green = pixel[1];
            const int red = pixel[2];
            yPlane[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>(ClampByte(((47 * red + 157 * green + 16 * blue + 128) >> 8) + 16));
        }
    });
    Concurrency::parallel_for(0u, height / 2, [&](std::uint32_t chromaY)
    {
        const std::uint32_t y = chromaY * 2;
        for (std::uint32_t x = 0; x < width; x += 2)
        {
            int red = 0;
            int green = 0;
            int blue = 0;
            for (std::uint32_t row = 0; row < 2; ++row)
            {
                for (std::uint32_t column = 0; column < 2; ++column)
                {
                    const std::uint8_t* pixel = bgra +
                        (static_cast<std::size_t>(y + row) * width + x + column) * 4;
                    blue += pixel[0];
                    green += pixel[1];
                    red += pixel[2];
                }
            }
            red /= 4;
            green /= 4;
            blue /= 4;
            const std::size_t uvOffset = static_cast<std::size_t>(chromaY) * width + x;
            uvPlane[uvOffset] = static_cast<std::uint8_t>(ClampByte(((-26 * red - 87 * green + 112 * blue + 128) >> 8) + 128));
            uvPlane[uvOffset + 1] = static_cast<std::uint8_t>(ClampByte(((112 * red - 102 * green - 10 * blue + 128) >> 8) + 128));
        }
    });
    return true;
}

bool ConvertNv12ToBgra(const std::uint8_t* nv12, std::size_t nv12Bytes,
                       std::uint32_t width, std::uint32_t height,
                       std::vector<std::uint8_t>& bgra)
{
    if (!nv12 || width == 0 || height == 0 || (width & 1) != 0 || (height & 1) != 0)
    {
        return false;
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    const std::size_t requiredBytes = pixelCount + pixelCount / 2;
    if (nv12Bytes < requiredBytes || pixelCount > (std::numeric_limits<std::size_t>::max() / 4))
    {
        return false;
    }

    bgra.resize(pixelCount * 4);
    const std::uint8_t* yPlane = nv12;
    const std::uint8_t* uvPlane = yPlane + pixelCount;
    Concurrency::parallel_for(0u, height, [&](std::uint32_t y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const int luminance = (std::max)(0, static_cast<int>(yPlane[static_cast<std::size_t>(y) * width + x]) - 16);
            const std::size_t uvOffset = static_cast<std::size_t>(y / 2) * width + (x & ~1u);
            const int blueDifference = static_cast<int>(uvPlane[uvOffset]) - 128;
            const int redDifference = static_cast<int>(uvPlane[uvOffset + 1]) - 128;
            const int scaledLuminance = 298 * luminance;
            std::uint8_t* pixel = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            pixel[0] = static_cast<std::uint8_t>(ClampByte((scaledLuminance + 541 * blueDifference + 128) >> 8));
            pixel[1] = static_cast<std::uint8_t>(ClampByte((scaledLuminance - 55 * blueDifference - 136 * redDifference + 128) >> 8));
            pixel[2] = static_cast<std::uint8_t>(ClampByte((scaledLuminance + 459 * redDifference + 128) >> 8));
            pixel[3] = 255;
        }
    });
    return true;
}
}
