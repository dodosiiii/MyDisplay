#pragma once

#include <cstdint>

namespace mydisplay::protocol
{
constexpr std::uint32_t kPairingMagic = 0x31524950;
constexpr std::uint32_t kPairingAcceptedMagic = 0x31504341;
constexpr std::uint32_t kPairingRejectedMagic = 0x31524A52;
constexpr std::uint16_t kSecureProtocolVersion = 1;
constexpr std::uint16_t kPairingPinLength = 6;

#pragma pack(push, 1)
struct PairingRequest
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t pinLength;
    char pin[kPairingPinLength];
    std::uint16_t reserved;
};

struct PairingResponse
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t status;
};
#pragma pack(pop)

static_assert(sizeof(PairingRequest) == 16);
static_assert(sizeof(PairingResponse) == 8);
}