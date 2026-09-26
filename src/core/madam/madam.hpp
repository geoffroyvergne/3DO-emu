#pragma once

#include <array>
#include <functional>
#include <span>
#include <string>
#include <unordered_set>

#include "common/types.hpp"
#include "core/bus/memory.hpp"

namespace core::input {
class PlayerBus;
}
namespace core::dsp {
class AudioDma;
}

namespace core::madam {

// MADAM register window (0x03300000): identity, memory configuration, the
// player-bus DMA, the cel engine and the video display list pointer. Other
// registers are plain storage that logs on first access.
//
// Control pads are not polled through a register. Portfolio's ControlPort
// driver, on each vertical blank, stores a destination pointer and length in
// the player DMA registers (its `RAMtofrPLAYER[]`) and sets MCTL.PLAYXEN;
// MADAM then streams the PBUS data into RAM and signals INT_DPLY.
// Offsets and behaviour follow Opera's opera_madam.c.
class Madam final : public MmioDevice {
public:
    static constexpr u32 kRegRevision = 0x000;  // read: revision; write: debug print port
    static constexpr u32 kRegMSysBits = 0x004;  // memory configuration, read-only
    static constexpr u32 kRegMctl = 0x008;
    static constexpr u32 kRegStatBits = 0x028;   // cel engine status (always idle here)
    static constexpr u32 kRegSprStart = 0x100;   // write: run the CCB list at NEXTCCB
    static constexpr u32 kRegSprStop = 0x104;
    static constexpr u32 kRegSprContinue = 0x108;
    static constexpr u32 kRegSprPause = 0x10C;
    static constexpr u32 kRegCeControl = 0x110;
    static constexpr u32 kRegRegCtl0 = 0x130;    // frame buffer line-pair modulos (read/write)
    static constexpr u32 kRegRegCtl1 = 0x134;    // clip width/height
    static constexpr u32 kRegRegCtl2 = 0x138;    // frame buffer read base
    static constexpr u32 kRegRegCtl3 = 0x13C;    // frame buffer write base
    static constexpr u32 kRegXyPosH = 0x140;
    static constexpr u32 kRegXyPosL = 0x144;
    static constexpr u32 kRegXbusDmaDest = 0x540;  // expansion-bus DMA target (Opera)
    static constexpr u32 kRegXbusDmaLen = 0x544;   // bytes - 4; idle = 0xFFFFFFFC
    static constexpr u32 kRegPlayerDest = 0x570;  // RAMtofrPLAYER[0]: input buffer
    static constexpr u32 kRegPlayerLen = 0x574;   // RAMtofrPLAYER[1]: bytes after the first word
    static constexpr u32 kRegPlayerOut = 0x578;   // RAMtofrPLAYER[2]: output buffer (advanced, unused)
    static constexpr u32 kRegVdlHead = 0x580;     // video display list for the next field
    static constexpr u32 kRegCurrentCcb = 0x5A0;
    static constexpr u32 kRegNextCcb = 0x5A4;
    static constexpr u32 kRegPlutData = 0x5A8;
    static constexpr u32 kRegPData = 0x5AC;
    static constexpr u32 kRegisterBytes = 0x800;

    static constexpr u32 kRevisionGreen = 0x01020000;  // Opera: MADAM_ID_GREEN_HARDWARE
    // 1 MB VRAM (bits 1:0 = 1), two 1 MB DRAM banks (1 << 5 | 1 << 3): Opera's
    // opera_mem_madam_red_sysbits() for the standard 2 MB + 1 MB console.
    static constexpr u32 kMSysBits2MbDram1MbVram = 0x29;

    static constexpr u32 kMctlClutXen = 0x00002000;  // VDL / CLUT DMA
    static constexpr u32 kMctlVscTxen = 0x00004000;  // video (bitmap) DMA
    static constexpr u32 kMctlPlayXen = 0x00008000;
    static constexpr u32 kPlayerIdleLen = 0xFFFFFFFC;

    // `ram` is the DRAM+VRAM block the cel engine reads CCBs from and draws into.
    Madam(Bus& bus, const input::PlayerBus& player_bus, std::span<u8> ram = {});

    void reset();

    // Called when a player DMA completes. CLIO will raise INT_DPLY from here
    // once it exists.
    void set_player_dma_done_handler(std::function<void()> handler) {
        player_dma_done_ = std::move(handler);
    }

    u32 mmio_read32(u32 offset) override;
    void mmio_write32(u32 offset, u32 value) override;

    // +0x400..+0x53F are the audio DMA FIFO registers.
    void attach_audio_dma(dsp::AudioDma* fifos) { fifos_ = fifos; }

    [[nodiscard]] u32 mctl() const { return regs_[kRegMctl >> 2]; }
    [[nodiscard]] u32 vdl_head() const { return regs_[kRegVdlHead >> 2]; }
    [[nodiscard]] u64 cels_drawn() const { return cels_drawn_; }

private:
    void run_player_dma();
    void run_cel_list();
    [[nodiscard]] u32 ram_word(u32 address) const;
    u32& reg(u32 offset) { return regs_[offset >> 2]; }
    void warn_unmodelled(const char* access, u32 offset);

    Bus& bus_;
    const input::PlayerBus& player_bus_;
    std::array<u32, kRegisterBytes / 4> regs_{};
    std::function<void()> player_dma_done_;
    std::unordered_set<u32> warned_offsets_;
    std::string debug_line_;  // characters written to the debug port

    // Cel engine state that persists from one CCB to the next.
    std::span<u8> ram_;
    dsp::AudioDma* fifos_ = nullptr;
    s32 x_pos_ = 0;
    s32 y_pos_ = 0;
    s32 hdx_ = 0, hdy_ = 0, vdx_ = 0, vdy_ = 0, hddx_ = 0, hddy_ = 0;
    u32 pixc_ = 0;
    std::array<u16, 32> plut_{};
    u64 cels_drawn_ = 0;
};

}  // namespace core::madam
