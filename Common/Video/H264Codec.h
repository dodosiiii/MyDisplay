#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include <mftransform.h>
#include <wrl/client.h>

namespace mydisplay::video
{
struct EncodedFrame
{
    std::uint64_t frameId = 0;
    std::vector<std::uint8_t> bytes;
};

struct DecodedFrame
{
    std::uint64_t frameId = 0;
    std::vector<std::uint8_t> bgra;
};

class H264Encoder
{
public:
    H264Encoder() = default;
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;
    ~H264Encoder();

    bool Initialize(std::uint32_t width, std::uint32_t height,
                    std::uint32_t framesPerSecond, std::uint32_t bitrate);
    bool EncodeBgra(const std::uint8_t* bgra, std::size_t bgraBytes,
                    std::uint64_t frameId, std::vector<EncodedFrame>& output);
    bool Poll(std::vector<EncodedFrame>& output);
    bool Flush(std::vector<EncodedFrame>& output);
    bool IsHardwareEncoder() const { return asynchronous_; }

private:
    bool SetOutputType();
    bool DrainOutput(std::vector<EncodedFrame>& output);
    bool PumpAsync(std::vector<EncodedFrame>& output, bool drainToEnd);
    bool ProcessAsyncOutput(std::vector<EncodedFrame>& output);

    struct PendingInput
    {
        std::uint64_t frameId = 0;
        std::vector<std::uint8_t> nv12;
    };

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator_;
    std::deque<PendingInput> pendingInputs_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t framesPerSecond_ = 0;
    std::uint32_t bitrate_ = 0;
    std::uint64_t currentFrameId_ = 0;
    std::uint32_t needInputEvents_ = 0;
    std::uint32_t haveOutputEvents_ = 0;
    bool asynchronous_ = false;
    bool mediaFoundationStarted_ = false;
    bool comInitialized_ = false;
};

class H264Decoder
{
public:
    H264Decoder() = default;
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;
    ~H264Decoder();

    bool Initialize(std::uint32_t width, std::uint32_t height,
                    std::uint32_t framesPerSecond);
    bool Decode(const std::uint8_t* encoded, std::size_t encodedBytes,
                std::uint64_t frameId, std::vector<DecodedFrame>& output);
    bool Flush(std::vector<DecodedFrame>& output);
    bool IsHardwareDecoder() const { return asynchronous_; }

private:
    bool DrainOutput(std::vector<DecodedFrame>& output);
    bool SetOutputType();
    bool PumpAsync(std::vector<DecodedFrame>& output, bool drainToEnd = false);
    bool ProcessAsyncOutput(std::vector<DecodedFrame>& output);

    struct PendingAccessUnit
    {
        std::uint64_t frameId = 0;
        std::vector<std::uint8_t> bytes;
    };

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator_;
    std::deque<PendingAccessUnit> pendingInputs_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t framesPerSecond_ = 0;
    std::uint64_t currentFrameId_ = 0;
    std::uint32_t needInputEvents_ = 0;
    std::uint32_t haveOutputEvents_ = 0;
    bool asynchronous_ = false;
    bool mediaFoundationStarted_ = false;
    bool comInitialized_ = false;
};

bool ConvertBgraToNv12(const std::uint8_t* bgra, std::size_t bgraBytes,
                       std::uint32_t width, std::uint32_t height,
                       std::vector<std::uint8_t>& nv12);
bool ConvertNv12ToBgra(const std::uint8_t* nv12, std::size_t nv12Bytes,
                       std::uint32_t width, std::uint32_t height,
                       std::vector<std::uint8_t>& bgra);
}
