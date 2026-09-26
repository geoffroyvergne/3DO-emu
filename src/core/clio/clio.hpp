#pragma once

#include <functional>
#include <unordered_set>
#include <vector>

#include "common/types.hpp"
#include "core/bus/memory.hpp"

namespace core::xbus {
class Xbus;
}
namespace core::dsp {
class Dspp;
class AudioDma;
}

namespace core::clio {

// CLIO register window (0x03400000): identity, reset reason, beam counter and
// the two interrupt banks that drive the ARM's FIQ line. Other registers are
// plain storage that logs on first access.
// Register layout: Portfolio OS `clio.h`; behaviour: Opera's opera_clio.c.
class Clio final : public MmioDevice {
public:
    static constexpr u32 kRegRevision = 0x000;
    static constexpr u32 kRegVInt0 = 0x008;
    static constexpr u32 kRegVInt1 = 0x00C;
    static constexpr u32 kRegCStatBits = 0x028;
    static constexpr u32 kRegWatchdog = 0x02C;
    static constexpr u32 kRegHCnt = 0x030;
    static constexpr u32 kRegVCnt = 0x034;
    static constexpr u32 kRegRandSample = 0x03C;
    static constexpr u32 kRegInt0Set = 0x040;
    static constexpr u32 kRegInt0Clr = 0x044;
    static constexpr u32 kRegInt0EnSet = 0x048;
    static constexpr u32 kRegInt0EnClr = 0x04C;
    static constexpr u32 kRegInt1Set = 0x060;
    static constexpr u32 kRegInt1Clr = 0x064;
    static constexpr u32 kRegInt1EnSet = 0x068;
    static constexpr u32 kRegInt1EnClr = 0x06C;
    static constexpr u32 kRegTimerFirst = 0x100;  // 16 x {count, reload}, 16-bit
    static constexpr u32 kRegTimerLast = 0x17C;
    static constexpr u32 kRegTimerCtlLoSet = 0x200;  // timers 0-7, 4 bits each
    static constexpr u32 kRegTimerCtlLoClr = 0x204;
    static constexpr u32 kRegTimerCtlHiSet = 0x208;  // timers 8-15
    static constexpr u32 kRegTimerCtlHiClr = 0x20C;
    static constexpr u32 kRegTimerSlack = 0x220;
    static constexpr u32 kRegExpCtl = 0x400;       // expansion bus control (xb_SetExpCtl)
    static constexpr u32 kExpCtlDmaOn = 0x800;     // XB_DMAON
    static constexpr u32 kExpCtlCpuHasBus = 0x80;  // XB_CPUHASXBUS: set again when a DMA ends
    static constexpr u32 kRegXbusDipir2 = 0x414;   // xb_DIPIR2
    // Opera returns XB_DipirNOSR ("dipir occurred before soft reset") here,
    // noting the CD-ROM dipir requires it.
    static constexpr u32 kDipir2Value = 0x4000;
    static constexpr u32 kRegFifoInit = 0x300;
    static constexpr u32 kRegDmaEnableSet = 0x304;
    static constexpr u32 kRegDmaEnableClr = 0x308;
    static constexpr u32 kDmaExpansionBus = 0x00100000;  // XBUS -> RAM (Opera)
    static constexpr u32 kInt0ExpansionDmaDone = 1u << 29;  // INT0_DEXINT
    static constexpr u32 kInt0Dsp = 1u << 11;               // INT0_DSPPINT
    static constexpr u32 kWindowBytes = 0x10000;

    static constexpr u32 kRevisionGreen = 0x02020000;  // CLIO_GREEN
    // Reset reason Opera always boots with: CSTAT_DIPIR_RESET ("start from CD").
    static constexpr u32 kResetReasonDipir = 0x40;

    static constexpr u32 kInt0VInt0 = 1u << 0;
    static constexpr u32 kInt0VInt1 = 1u << 1;
    static constexpr u32 kInt0ExpansionBus = 1u << 2;  // INT0_EXINT
    // Int0 bit 31 summarises bank 1: set while any *enabled* Int1 source is
    // pending. The kernel's FIQ dispatcher relies on this (it clears bit 31
    // and expects it to stay clear when nothing enabled is pending in Int1);
    // Opera instead sets it for any pending Int1 bit, which makes this BIOS
    // spin forever in its dispatcher.
    static constexpr u32 kInt0SecondBank = 1u << 31;

    explicit Clio(std::function<void(bool)> fiq_line);

    void reset();

    // Video timing, driven by the system once per scanline.
    void set_beam(u32 line, bool odd_field);
    [[nodiscard]] u32 vint0_line() const { return reg(kRegVInt0) & 0x7FF; }
    [[nodiscard]] u32 vint1_line() const { return reg(kRegVInt1) & 0x7FF; }

    // Advances the 16 hardware timers by `cpu_cycles` of emulated time.
    void advance_timers(u32 cpu_cycles);

    // Interrupt sources.
    void raise_int0(u32 bits);
    void raise_int1(u32 bits);

    // Routes +0x500..+0x5FF to the expansion bus.
    void attach_xbus(xbus::Xbus* xbus) { xbus_ = xbus; }

    // Routes the DSP window (+0x17D0.. semaphore/control, +0x1800.. code,
    // +0x3000.. input registers, +0x3800.. output registers) and the audio
    // DMA enables.
    void attach_dsp(dsp::Dspp* dsp, dsp::AudioDma* fifos) {
        dsp_ = dsp;
        fifos_ = fifos;
    }

    // Performs an expansion-bus DMA when software enables it (the transfer
    // itself needs MADAM's DMA registers and RAM, so the system provides it).
    void set_xbus_dma_handler(std::function<void()> handler) { xbus_dma_ = std::move(handler); }

    u32 mmio_read32(u32 offset) override;
    void mmio_write32(u32 offset, u32 value) override;
    [[nodiscard]] bool access_aborts(u32 offset) const override;

private:
    [[nodiscard]] u32 reg(u32 offset) const { return regs_[offset >> 2]; }
    u32& reg(u32 offset) { return regs_[offset >> 2]; }
    void update_fiq();
    [[nodiscard]] u32 int0_pending() const;
    void warn_unmodelled(const char* access, u32 offset);

    std::function<void(bool)> fiq_line_;
    xbus::Xbus* xbus_ = nullptr;
    dsp::Dspp* dsp_ = nullptr;
    dsp::AudioDma* fifos_ = nullptr;
    [[nodiscard]] bool dsp_read(u32 offset, u32& value);
    bool dsp_write(u32 offset, u32 value);
    void update_dma_enable();
    std::function<void()> xbus_dma_;
    std::vector<u32> regs_;
    void run_timer_scan();

    u32 rng_state_ = 0x12345678;
    u64 timer_ticks_ = 0;  // 25 MHz ticks not yet consumed by a timer scan
    std::unordered_set<u32> warned_offsets_;
};

}  // namespace core::clio
