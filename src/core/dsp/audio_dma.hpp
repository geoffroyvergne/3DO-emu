#pragma once

#include <array>
#include <functional>
#include <span>

#include "common/types.hpp"

namespace core::dsp {

// The DMA FIFOs between RAM and the DSP: 13 input channels (RAM -> DSP) and
// 4 output channels (DSP -> RAM). Registers live in MADAM's window at
// +0x400 (inputs, 16 bytes per channel) and +0x500 (outputs):
//   +0 current address, +4 current length, +8 next address, +C next length
// Lengths are written as bytes - 4. When a channel runs dry it raises its
// interrupt (Int0 bit 16 + ch for inputs, 12 + ch for outputs) and reloads
// from "next" if its DMA enable bit is set. Behaviour follows Opera's
// opera_clio.c fifo functions.
class AudioDma {
public:
    static constexpr u32 kInputChannels = 13;
    static constexpr u32 kOutputChannels = 4;
    static constexpr u32 kFirstRegister = 0x400;
    static constexpr u32 kLastRegister = 0x53F;

    AudioDma(std::span<u8> ram, std::function<void(u32)> raise_int0)
        : ram_(ram), raise_int0_(std::move(raise_int0)) {}

    void reset();

    // MADAM register window +0x400..+0x53F.
    [[nodiscard]] u32 read_register(u32 offset) const;
    void write_register(u32 offset, u32 value);

    // CLIO's DMA enable register (SetDMAEnable / ClrDMAEnable) and FifoInit.
    void set_dma_enable(u32 bits) { dma_enable_ = bits; }
    void init_channels(u32 bits);

    // DSP side.
    u16 pop_input(u32 channel);           // consumes one sample (may reload / interrupt)
    [[nodiscard]] u16 peek_input(u32 channel) const;
    [[nodiscard]] u16 input_status(u32 channel) const;
    void push_output(u32 channel, u16 value);
    [[nodiscard]] u16 output_status(u32 channel) const;

private:
    struct Buffer {
        u32 address = 0;
        u32 length = 0;
    };
    struct Channel {
        Buffer current;
        Buffer next;
        u32 position = 0;  // bytes consumed from `current`
    };

    [[nodiscard]] u16 read16(u32 address) const;
    void write16(u32 address, u16 value);
    [[nodiscard]] bool enabled(u32 channel) const { return (dma_enable_ >> channel) & 1; }

    std::span<u8> ram_;
    std::function<void(u32)> raise_int0_;
    std::array<Channel, kInputChannels> inputs_{};
    std::array<Channel, kOutputChannels> outputs_{};
    u32 dma_enable_ = 0;
};

}  // namespace core::dsp
