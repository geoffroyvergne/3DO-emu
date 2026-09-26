#include "core/dsp/audio_dma.hpp"

namespace core::dsp {

void AudioDma::reset() {
    inputs_ = {};
    outputs_ = {};
    dma_enable_ = 0;
}

u16 AudioDma::read16(u32 address) const {
    if (address + 1 >= ram_.size()) return 0;
    return static_cast<u16>((ram_[address] << 8) | ram_[address + 1]);
}

void AudioDma::write16(u32 address, u16 value) {
    if (address + 1 >= ram_.size()) return;
    ram_[address] = static_cast<u8>(value >> 8);
    ram_[address + 1] = static_cast<u8>(value);
}

u32 AudioDma::read_register(u32 offset) const {
    const bool input = offset < 0x500;
    const u32 channel = (offset >> 4) & 0xF;
    if (input ? channel >= kInputChannels : channel >= kOutputChannels) return 0;
    const Channel& c = input ? inputs_[channel] : outputs_[channel];
    switch (offset & 0xF) {
        case 0x0: return c.current.address + c.position;
        case 0x4: return c.current.length - c.position;
        case 0x8: return c.next.address;
        default: return c.next.length;
    }
}

void AudioDma::write_register(u32 offset, u32 value) {
    const bool input = offset < 0x500;
    const u32 channel = (offset >> 4) & 0xF;
    if (input ? channel >= kInputChannels : channel >= kOutputChannels) return;
    Channel& c = input ? inputs_[channel] : outputs_[channel];
    const u32 length = (input && value == 0) ? 0 : value + 4;
    switch (offset & 0xF) {
        case 0x0:
            c.current.address = value;
            if (input) c.next.address = 0;
            break;
        case 0x4:
            c.current.length = length;
            if (input) c.next.length = 0;
            break;
        case 0x8: c.next.address = value; break;
        default: c.next.length = length; break;
    }
}

void AudioDma::init_channels(u32 bits) {
    dma_enable_ &= ~bits;
    for (u32 i = 0; i < kInputChannels; ++i)
        if (bits & (1u << i)) inputs_[i] = {};
    for (u32 i = 0; i < kOutputChannels; ++i)
        if (bits & (1u << (i + 16))) outputs_[i] = {};
}

u16 AudioDma::pop_input(u32 channel) {
    if (channel >= kInputChannels) return 0;
    Channel& c = inputs_[channel];
    if (c.current.address == 0) return 0;
    if (c.position < c.current.length) {
        const u16 value = read16(c.current.address + c.position);
        c.position += 2;
        return value;
    }
    // Buffer drained: interrupt, then continue with the queued buffer if any.
    c.position = 0;
    if (raise_int0_) raise_int0_(1u << (channel + 16));
    if (c.next.address != 0 && enabled(channel)) {
        c.current = c.next;
        const u16 value = read16(c.current.address);
        c.position = 2;
        return value;
    }
    c.current.address = 0;
    return 0;
}

u16 AudioDma::peek_input(u32 channel) const {
    if (channel >= kInputChannels) return 0;
    const Channel& c = inputs_[channel];
    return read16(c.current.address + c.position);
}

u16 AudioDma::input_status(u32 channel) const {
    return (channel < kInputChannels && inputs_[channel].current.address != 0) ? 2 : 0;
}

void AudioDma::push_output(u32 channel, u16 value) {
    if (channel >= kOutputChannels) return;
    Channel& c = outputs_[channel];
    if (c.current.address == 0) return;
    if (c.position < c.current.length) {
        write16(c.current.address + c.position, value);
        c.position += 2;
        return;
    }
    c.position = 0;
    if (raise_int0_) raise_int0_(1u << (channel + 12));
    if (c.next.address != 0 && enabled(channel))
        c.current = c.next;
    else
        c.current.address = 0;
}

u16 AudioDma::output_status(u32 channel) const {
    return (channel < kOutputChannels && outputs_[channel].current.address != 0) ? 1 : 0;
}

}  // namespace core::dsp
