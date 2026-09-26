#include "core/input/pbus.hpp"

#include <algorithm>

#include "common/log.hpp"

namespace core::input {

namespace {

// Joypad report, MSB first (1 = pressed):
//   byte 0: 1 0 0 D U R L A    (bit 7 is the joypad ID bit, 0x80)
//   byte 1: B C P X RT LT 0 0
constexpr u8 kJoypadId = 0x80;
constexpr u8 kEndOfChain = 0xFF;

u8 bit(const PadState& pad, PadButton button, u32 shift) {
    return static_cast<u8>(pad.pressed(button) ? 1u << shift : 0u);
}

// A physical pad cannot report opposite directions at once, and the PBUS ID
// scheme relies on that (see the PBUS patent quoted in Opera's pbus.txt).
// Keyboards can, so cancel such pairs.
PadState without_opposites(PadState pad) {
    if (pad.pressed(PadButton::Up) && pad.pressed(PadButton::Down)) {
        pad.set(PadButton::Up, false);
        pad.set(PadButton::Down, false);
    }
    if (pad.pressed(PadButton::Left) && pad.pressed(PadButton::Right)) {
        pad.set(PadButton::Left, false);
        pad.set(PadButton::Right, false);
    }
    return pad;
}

}  // namespace

void PlayerBus::set_pad_count(std::size_t count) {
    if (count > kMaxPads) {
        Log::warn("PBUS: {} pads requested, chain limited to {}", count, kMaxPads);
        count = kMaxPads;
    }
    pad_count_ = count;
}

void PlayerBus::set_pad(std::size_t index, PadState state) {
    if (index < kMaxPads) pads_[index] = state;
}

std::vector<u8> PlayerBus::serialize() const {
    std::vector<u8> stream;
    stream.reserve(pad_count_ * 2 + 4);

    for (std::size_t i = 0; i < pad_count_; ++i) {
        const PadState pad = without_opposites(pads_[i]);
        stream.push_back(static_cast<u8>(kJoypadId | bit(pad, PadButton::Down, 4) |
                                         bit(pad, PadButton::Up, 3) | bit(pad, PadButton::Right, 2) |
                                         bit(pad, PadButton::Left, 1) | bit(pad, PadButton::A, 0)));
        stream.push_back(static_cast<u8>(bit(pad, PadButton::B, 7) | bit(pad, PadButton::C, 6) |
                                         bit(pad, PadButton::P, 5) | bit(pad, PadButton::X, 4) |
                                         bit(pad, PadButton::R, 3) | bit(pad, PadButton::L, 2)));
    }

    // At least one end-of-chain byte, then pad to the DMA's word granularity.
    do stream.push_back(kEndOfChain);
    while (stream.size() % 4 != 0);
    return stream;
}

}  // namespace core::input
