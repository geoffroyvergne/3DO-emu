// Tests the PBUS encoding and MADAM's player DMA, driven through MMIO like the OS does.
#include <utility>
#include <vector>

#include "core/bus/memory.hpp"
#include "core/input/pbus.hpp"
#include "core/madam/madam.hpp"
#include "test_common.hpp"

namespace {

using core::Memory;
using core::input::PadButton;
using core::input::PadState;
using core::input::PlayerBus;
using core::madam::Madam;

constexpr u32 kMadamBase = 0x03300000;

PadState pad_with(std::initializer_list<PadButton> buttons) {
    PadState pad;
    for (PadButton b : buttons) pad.set(b, true);
    return pad;
}

void test_idle_pad_encoding() {
    PlayerBus bus;
    const auto stream = bus.serialize();
    CHECK_EQ(stream.size(), 4u);
    CHECK_EQ(stream[0], 0x80u);  // joypad ID bit, nothing pressed
    CHECK_EQ(stream[1], 0x00u);
    CHECK_EQ(stream[2], 0xFFu);  // end of chain, padded to a word
    CHECK_EQ(stream[3], 0xFFu);
}

void test_each_button_bit() {
    const std::pair<PadButton, std::pair<u8, u8>> expected[] = {
        {PadButton::Down, {0x90, 0x00}}, {PadButton::Up, {0x88, 0x00}},
        {PadButton::Right, {0x84, 0x00}}, {PadButton::Left, {0x82, 0x00}},
        {PadButton::A, {0x81, 0x00}},     {PadButton::B, {0x80, 0x80}},
        {PadButton::C, {0x80, 0x40}},     {PadButton::P, {0x80, 0x20}},
        {PadButton::X, {0x80, 0x10}},     {PadButton::R, {0x80, 0x08}},
        {PadButton::L, {0x80, 0x04}},
    };
    for (const auto& [button, bytes] : expected) {
        PlayerBus bus;
        bus.set_pad(0, pad_with({button}));
        const auto stream = bus.serialize();
        CHECK_EQ(stream[0], bytes.first);
        CHECK_EQ(stream[1], bytes.second);
    }
}

void test_opposite_directions_cancel() {
    PlayerBus bus;
    bus.set_pad(0, pad_with({PadButton::Up, PadButton::Down, PadButton::Left, PadButton::A}));
    const auto stream = bus.serialize();
    CHECK_EQ(stream[0], 0x83u);  // Left + A only
}

void test_daisy_chain_order() {
    PlayerBus bus;
    bus.set_pad_count(3);
    bus.set_pad(0, pad_with({PadButton::A}));
    bus.set_pad(1, pad_with({PadButton::B}));
    bus.set_pad(2, pad_with({PadButton::L}));
    const auto stream = bus.serialize();
    CHECK_EQ(stream.size(), 8u);
    CHECK_EQ(stream[0], 0x81u);
    CHECK_EQ(stream[3], 0x80u);  // pad 2, byte 1: B
    CHECK_EQ(stream[5], 0x04u);  // pad 3, byte 1: L
    CHECK_EQ(stream[6], 0xFFu);
    CHECK_EQ(stream[7], 0xFFu);

    bus.set_pad_count(0);
    CHECK_EQ(bus.serialize().size(), 4u);  // empty chain: one word of 0xFF
}

// Runs the same register sequence Portfolio's EnableControlPort() does.
struct Rig {
    Memory memory;
    PlayerBus pbus;
    Madam madam{memory, pbus};
    int dma_done = 0;

    Rig() {
        memory.attach(core::MmioRegion::Madam, &madam);
        madam.set_player_dma_done_handler([this] { ++dma_done; });
    }

    void trigger(u32 dest, u32 dma_size) {
        memory.write32(kMadamBase + Madam::kRegPlayerDest, dest);
        memory.write32(kMadamBase + Madam::kRegPlayerOut, 0x2000);
        memory.write32(kMadamBase + Madam::kRegPlayerLen, dma_size - 4);
        const u32 mctl = memory.read32(kMadamBase + Madam::kRegMctl);
        memory.write32(kMadamBase + Madam::kRegMctl, mctl | Madam::kMctlPlayXen);
    }
};

void test_player_dma_through_mmio() {
    Rig rig;
    rig.pbus.set_pad(0, pad_with({PadButton::Right, PadButton::C}));
    rig.memory.write32(0x1010, 0x12345678);  // just past the transfer: must survive

    rig.trigger(0x1000, 16);
    CHECK_EQ(rig.memory.read32(0x1000), 0xFFFFFFFFu);  // leading filler word
    CHECK_EQ(rig.memory.read32(0x1004), 0x8440FFFFu);  // Right | C, end of chain
    CHECK_EQ(rig.memory.read32(0x1008), 0xFFFFFFFFu);  // rest of the buffer: all ones
    CHECK_EQ(rig.memory.read32(0x100C), 0xFFFFFFFFu);
    CHECK_EQ(rig.memory.read32(0x1010), 0x12345678u);

    CHECK_EQ(rig.dma_done, 1);
    CHECK_EQ(rig.memory.read32(kMadamBase + Madam::kRegMctl) & Madam::kMctlPlayXen, 0u);
    CHECK_EQ(rig.memory.read32(kMadamBase + Madam::kRegPlayerLen), Madam::kPlayerIdleLen);
    CHECK_EQ(rig.memory.read32(kMadamBase + Madam::kRegPlayerDest), 0x1010u);

    // The next frame sees the new state.
    rig.pbus.set_pad(0, pad_with({PadButton::P}));
    rig.trigger(0x1000, 16);
    CHECK_EQ(rig.memory.read32(0x1004), 0x8020FFFFu);
    CHECK_EQ(rig.dma_done, 2);
}

void test_short_dma_truncates_stream() {
    Rig rig;
    rig.pbus.set_pad_count(4);
    rig.memory.write32(0x1008, 0xCAFEF00D);
    rig.trigger(0x1000, 8);  // room for the filler word and one data word only
    CHECK_EQ(rig.memory.read32(0x1004), 0x80008000u);  // pads 1 and 2
    CHECK_EQ(rig.memory.read32(0x1008), 0xCAFEF00Du);  // untouched
}

void test_other_registers_are_storage() {
    Rig rig;
    rig.memory.write32(kMadamBase + 0x100, 0xABCD);
    CHECK_EQ(rig.memory.read32(kMadamBase + 0x100), 0xABCDu);
    CHECK_EQ(rig.dma_done, 0);
}

}  // namespace

int main() {
    test_idle_pad_encoding();
    test_each_button_bit();
    test_opposite_directions_cancel();
    test_daisy_chain_order();
    test_player_dma_through_mmio();
    test_short_dma_truncates_stream();
    test_other_registers_are_storage();
    return report("input_test");
}
