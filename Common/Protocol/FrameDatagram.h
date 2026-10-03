#pragma once

#include <cstddef>
#include <cstdint>

namespace mydisplay::protocol
{
constexpr std::uint32_t kFrameMagic = 0x3150444D;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kMaxUdpDatagramBytes = 1200;
constexpr std::uint16_t kPixelFormatBgra8 = 1;
constexpr std::uint16_t kPixelFormatH264 = 2;
constexpr std::uint16_t kMaxFrameWidth = 1920;
constexpr std::uint16_t kMaxFrameHeight = 1080;
constexpr std::size_t kMaxEncodedFrameBytes = 16 * 1024 * 1024;

#pragma pack(push, 1)
struct FrameDatagramHeader
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t headerBytes;
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t pixelFormat;
    std::uint16_t flags;
    std::uint64_t frameId;
    std::uint16_t fragmentIndex;
    std::uint16_t fragmentCount;
    std::uint16_t payloadBytes;
    std::uint16_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(FrameDatagramHeader) == 32);
static_assert(kMaxUdpDatagramBytes > sizeof(FrameDatagramHeader));

constexpr std::size_t kMaxFragmentPayloadBytes = kMaxUdpDatagramBytes - sizeof(FrameDatagramHeader);

inline bool IsValid(const FrameDatagramHeader& header, std::size_t datagramBytes)
{
    if (header.magic != kFrameMagic || header.version != kProtocolVersion ||
        header.headerBytes != sizeof(FrameDatagramHeader) ||
        (header.pixelFormat != kPixelFormatBgra8 && header.pixelFormat != kPixelFormatH264) ||
        header.width == 0 || header.height == 0 || header.width > kMaxFrameWidth ||
        header.height > kMaxFrameHeight || header.fragmentCount == 0 ||
        header.fragmentIndex >= header.fragmentCount || header.payloadBytes == 0 ||
        header.payloadBytes > kMaxFragmentPayloadBytes || header.reserved != 0 ||
        datagramBytes != sizeof(FrameDatagramHeader) + header.payloadBytes)
    {
        return false;
    }

    if (header.pixelFormat == kPixelFormatBgra8)
    {
        const std::uint64_t frameBytes = static_cast<std::uint64_t>(header.width) * header.height * 4;
        const std::uint64_t expectedFragments =
            (frameBytes + kMaxFragmentPayloadBytes - 1) / kMaxFragmentPayloadBytes;
        return expectedFragments == header.fragmentCount;
    }

    const std::uint64_t maximumFragments =
        (kMaxEncodedFrameBytes + kMaxFragmentPayloadBytes - 1) / kMaxFragmentPayloadBytes;
    if (header.fragmentCount > maximumFragments ||
        (header.fragmentIndex + 1 < header.fragmentCount &&
         header.payloadBytes != kMaxFragmentPayloadBytes))
    {
        return false;
    }
    const std::uint64_t encodedBytes =
        static_cast<std::uint64_t>(header.fragmentCount - 1) * kMaxFragmentPayloadBytes +
        (header.fragmentIndex + 1 == header.fragmentCount ? header.payloadBytes : kMaxFragmentPayloadBytes);
    return encodedBytes <= kMaxEncodedFrameBytes;
}
}