#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "common/types.hpp"

namespace core::input {

// Logical 3DO control pad buttons. These bit positions are our own; the wire
// encoding lives in PlayerBus::serialize().
enum class PadButton : u16 {
    Up = 1 << 0,
    Down = 1 << 1,
    Left = 1 << 2,
    Right = 1 << 3,
    A = 1 << 4,
    B = 1 << 5,
    C = 1 << 6,
    P = 1 << 7,  // Play/Pause
    X = 1 << 8,  // Stop
    L = 1 << 9,  // left shoulder
    R = 1 << 10, // right shoulder
};

struct PadState {
    u16 buttons = 0;

    [[nodiscard]] bool pressed(PadButton b) const { return (buttons & static_cast<u16>(b)) != 0; }
    void set(PadButton b, bool down) {
        if (down)
            buttons = static_cast<u16>(buttons | static_cast<u16>(b));
        else
            buttons = static_cast<u16>(buttons & ~static_cast<u16>(b));
    }
};

// The PBUS ("player bus"): control pads daisy-chained off the console, read
// by MADAM's player DMA as one bit stream. Format and framing follow the
// Opera emulator's PBUS implementation (libopera/opera_pbus.c, pbus.txt).
class PlayerBus {
public:
    static constexpr std::size_t kMaxPads = 8;

    // Number of pads plugged into the chain (they are always contiguous).
    void set_pad_count(std::size_t count);
    [[nodiscard]] std::size_t pad_count() const { return pad_count_; }

    void set_pad(std::size_t index, PadState state);
    [[nodiscard]] PadState pad(std::size_t index) const { return pads_[index]; }

    // Builds the stream the player DMA delivers: each pad's 2-byte report in
    // chain order, then 0xFF end-of-chain bytes up to a word boundary.
    [[nodiscard]] std::vector<u8> serialize() const;

private:
    std::array<PadState, kMaxPads> pads_{};
    std::size_t pad_count_ = 1;
};

}  // namespace core::input
