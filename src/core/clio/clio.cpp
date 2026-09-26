#include "core/clio/clio.hpp"

#include "common/log.hpp"

#include <cstdlib>
#include "core/bus/mmio_names.hpp"
#include "core/xbus/xbus.hpp"
#include "core/dsp/audio_dma.hpp"
#include "core/dsp/dspp.hpp"

namespace core::clio {

namespace {

constexpr u32 kLineMask = 0x7FF;
constexpr u32 kFieldBit = 0x800;
constexpr u32 kCStatSoftReset = 0x10;
constexpr u32 kCStatClearDipir = 0x20;
constexpr u32 kCStatDipirReset = 0x40;

// Timer control nibble (Opera: DECREMENT / RELOAD / CASCADE).
constexpr u32 kTimerDecrement = 0x1;
constexpr u32 kTimerReload = 0x2;
constexpr u32 kTimerCascade = 0x4;
// One timer scan: 16 slots of 4 ticks at 25 MHz, then TimerSlack ticks.
constexpr u32 kTimerSlotTicks = 4;
constexpr u32 kTimerCount = 16;

constexpr bool is_timer_counter(u32 offset) {
    return offset >= Clio::kRegTimerFirst && offset <= Clio::kRegTimerLast;
}

}  // namespace

Clio::Clio(std::function<void(bool)> fiq_line)
    : fiq_line_(std::move(fiq_line)), regs_(kWindowBytes / 4) {
    reset();
}

void Clio::reset() {
    std::ranges::fill(regs_, 0u);
    // Power-on values Opera seeds (opera_clio_init).
    reg(kRegCStatBits) = kResetReasonDipir;
    reg(0x400) = 0x80;  // expansion bus
    reg(0x220) = 64;    // timer slack
    reg(kRegInt0EnSet) = kInt0SecondBank;
    timer_ticks_ = 0;
    warned_offsets_.clear();
    update_fiq();
}

void Clio::advance_timers(u32 cpu_cycles) {
    // The CPU runs at 12.5 MHz, the timer logic at 25 MHz.
    timer_ticks_ += u64{cpu_cycles} * 2;
    const u64 scan_ticks = kTimerSlotTicks * kTimerCount + (reg(kRegTimerSlack) & 0x3FF);
    while (timer_ticks_ >= scan_ticks) {
        timer_ticks_ -= scan_ticks;
        run_timer_scan();
    }
}

void Clio::run_timer_scan() {
    bool carry = false;  // underflow of the previous timer, for cascading
    for (u32 t = 0; t < kTimerCount; ++t) {
        u32& control = reg(t < 8 ? kRegTimerCtlLoSet : kRegTimerCtlHiSet);
        const u32 shift = (t & 7) * 4;
        const u32 flags = (control >> shift) & 0xF;
        bool underflow = false;

        if ((flags & kTimerDecrement) && (!(flags & kTimerCascade) || carry)) {
            u32& count = reg(kRegTimerFirst + t * 8);
            if ((count & 0xFFFF) == 0) {
                underflow = true;
                // Odd timers interrupt: timer 1 -> Int0 bit 10 ... timer 15 -> bit 3.
                if (t & 1) reg(kRegInt0Set) |= 1u << (10 - (t >> 1));
                if (flags & kTimerReload) {
                    count = reg(kRegTimerFirst + t * 8 + 4) & 0xFFFF;
                } else {
                    count = 0xFFFF;
                    control &= ~(kTimerDecrement << shift);
                }
            } else {
                count = (count & 0xFFFF) - 1;
            }
        }
        carry = underflow;
    }
    update_fiq();
}

void Clio::set_beam(u32 line, bool odd_field) {
    reg(kRegVCnt) = (line & kLineMask) | (odd_field ? kFieldBit : 0u);
}

void Clio::raise_int0(u32 bits) {
    reg(kRegInt0Set) |= bits;
    update_fiq();
}

void Clio::raise_int1(u32 bits) {
    reg(kRegInt1Set) |= bits;
    update_fiq();
}

u32 Clio::int0_pending() const {
    const bool bank1 = (reg(kRegInt1Set) & reg(kRegInt1EnSet)) != 0;
    return (reg(kRegInt0Set) & ~kInt0SecondBank) | (bank1 ? kInt0SecondBank : 0u);
}

void Clio::update_fiq() {
    const bool asserted = (int0_pending() & reg(kRegInt0EnSet)) != 0;
    if (fiq_line_) fiq_line_(asserted);
}

namespace {

constexpr bool is_xbus(u32 offset) {
    return offset >= xbus::Xbus::kSelect && offset < xbus::Xbus::kEnd;
}

}  // namespace

// DSP window mapping (Opera's opera_clio_peek/poke).
bool Clio::dsp_read(u32 offset, u32& value) {
    if (!dsp_) return false;
    if (offset == 0x17D0) {
        value = dsp_->read_semaphore();
        return true;
    }
    if (offset >= 0x3800 && offset <= 0x3BFF) {  // two output words per ARM word
        const u32 a = (((offset - 0x3800) >> 1) & 0xFF) + 0x300;
        value = (u32{dsp_->read_output(a)} << 16) | dsp_->read_output(a + 1);
        return true;
    }
    if (offset >= 0x3C00 && offset <= 0x3FFF) {
        value = dsp_->read_output((((offset - 0x3C00) >> 2) & 0xFF) + 0x300);
        return true;
    }
    return false;
}

bool Clio::dsp_write(u32 offset, u32 value) {
    if (!dsp_) return false;
    if (offset == 0x17D0) { dsp_->write_semaphore(value); return true; }
    if (offset == 0x17E8) { dsp_->reset(); return true; }
    if (offset == 0x17FC) { dsp_->set_running(value > 0); return true; }
    if (offset >= 0x1800 && offset <= 0x1FFF) {  // code: two DSP words per ARM word
        const u32 a = ((offset & ~0x400u) - 0x1800) >> 1;
        dsp_->write_code(a, static_cast<u16>(value >> 16));
        dsp_->write_code(a + 1, static_cast<u16>(value));
        return true;
    }
    if (offset >= 0x2000 && offset <= 0x2FFF) {  // code: one DSP word per ARM word
        dsp_->write_code(((offset & ~0x800u) - 0x2000) >> 2, static_cast<u16>(value));
        return true;
    }
    if (offset >= 0x3000 && offset <= 0x33FF) {  // input registers, two per ARM word
        const u32 a = ((offset - 0x3000) >> 1) & 0xFF;
        dsp_->write_input(a, static_cast<u16>(value >> 16));
        dsp_->write_input(a + 1, static_cast<u16>(value));
        return true;
    }
    if (offset >= 0x3400 && offset <= 0x37FF) {
        dsp_->write_input(((offset - 0x3400) >> 2) & 0xFF, static_cast<u16>(value));
        return true;
    }
    return false;
}

void Clio::update_dma_enable() {
    if (fifos_) fifos_->set_dma_enable(reg(kRegDmaEnableSet));
}

bool Clio::access_aborts(u32 offset) const {
    return xbus_ && is_xbus(offset) && xbus_->access_aborts(offset);
}

u32 Clio::mmio_read32(u32 offset) {
    if (offset >= kWindowBytes) {
        warn_unmodelled("read", offset);
        return 0;
    }
    if (xbus_ && is_xbus(offset)) return xbus_->read(offset);
    if (u32 value = 0; dsp_read(offset, value)) return value;
    switch (offset) {
        case kRegRevision: return kRevisionGreen;
        case kRegVInt0:
        case kRegVInt1: return reg(offset) & kLineMask;
        case kRegCStatBits:
        case kRegWatchdog:
        case kRegHCnt:
        case kRegVCnt: return reg(offset);
        case kRegRandSample:
            // xorshift32; real hardware samples video noise.
            rng_state_ ^= rng_state_ << 13;
            rng_state_ ^= rng_state_ >> 17;
            rng_state_ ^= rng_state_ << 5;
            return rng_state_;
        // Set/clear pairs read back the same register.
        case kRegInt0Set:
        case kRegInt0Clr: return int0_pending();
        case kRegInt0EnSet:
        case kRegInt0EnClr: return reg(kRegInt0EnSet);
        case kRegInt1Set:
        case kRegInt1Clr: return reg(kRegInt1Set);
        case kRegInt1EnSet:
        case kRegInt1EnClr: return reg(kRegInt1EnSet);
        case kRegTimerCtlLoSet:
        case kRegTimerCtlLoClr: return reg(kRegTimerCtlLoSet);
        case kRegTimerCtlHiSet:
        case kRegTimerCtlHiClr: return reg(kRegTimerCtlHiSet);
        case kRegTimerSlack: return reg(offset);
        case kRegDmaEnableSet:
        case kRegDmaEnableClr: return reg(kRegDmaEnableSet);
        case kRegXbusDipir2: return kDipir2Value;
        default:
            if (is_timer_counter(offset)) return reg(offset) & 0xFFFF;
            warn_unmodelled("read", offset);
            return reg(offset);
    }
}

namespace {
// Debugging aid: EMU_TRACE_DSP=1 logs DSP control and audio DMA enable writes.
bool trace_dsp() {
    static const bool enabled = std::getenv("EMU_TRACE_DSP") != nullptr;
    return enabled;
}
}  // namespace

void Clio::mmio_write32(u32 offset, u32 value) {
    if (trace_dsp() && (offset == 0x17FC || offset == 0x17E8 || offset == 0x17D0 ||
                        offset == kRegFifoInit || offset == kRegDmaEnableSet || offset == kRegDmaEnableClr))
        Log::info("DSP ctl write +0x{:04X} = 0x{:08X}", offset, value);
    if (offset >= kWindowBytes) {
        warn_unmodelled("write", offset);
        return;
    }
    if (xbus_ && is_xbus(offset)) {
        xbus_->write(offset, value);
        return;
    }
    if (dsp_write(offset, value)) return;
    switch (offset) {
        case kRegRevision: return;  // read-only
        case kRegVInt0:
        case kRegVInt1: reg(offset) = value & kLineMask; return;
        case kRegCStatBits:
            if (value & kCStatSoftReset)
                Log::warn("CLIO: soft reset requested (not implemented)");
            reg(offset) = value & ~(kCStatSoftReset | kCStatClearDipir);
            if (value & kCStatClearDipir) reg(offset) &= ~kCStatDipirReset;
            return;
        case kRegWatchdog: reg(offset) = value; return;  // watchdog never fires
        case kRegInt0Set: reg(kRegInt0Set) |= value; break;
        case kRegInt0Clr: reg(kRegInt0Set) &= ~value; break;  // bit 31 is derived
        case kRegInt0EnSet: reg(kRegInt0EnSet) |= value; break;
        case kRegInt0EnClr:
            reg(kRegInt0EnSet) &= ~value;
            reg(kRegInt0EnSet) |= kInt0SecondBank;  // always enabled (Opera)
            break;
        case kRegInt1Set: reg(kRegInt1Set) |= value; break;
        case kRegInt1Clr: reg(kRegInt1Set) &= ~value; break;
        case kRegInt1EnSet: reg(kRegInt1EnSet) |= value; break;
        case kRegInt1EnClr: reg(kRegInt1EnSet) &= ~value; break;
        case kRegTimerCtlLoSet: reg(kRegTimerCtlLoSet) |= value; return;
        case kRegTimerCtlLoClr: reg(kRegTimerCtlLoSet) &= ~value; return;
        case kRegTimerCtlHiSet: reg(kRegTimerCtlHiSet) |= value; return;
        case kRegTimerCtlHiClr: reg(kRegTimerCtlHiSet) &= ~value; return;
        case kRegTimerSlack: reg(offset) = value & 0x3FF; return;
        case kRegFifoInit:
            reg(kRegDmaEnableSet) &= ~value;
            if (fifos_) fifos_->init_channels(value);
            update_dma_enable();
            return;
        case kRegDmaEnableSet:
            reg(kRegDmaEnableSet) |= value;
            update_dma_enable();
            if (value & kDmaExpansionBus) {
                reg(kRegDmaEnableSet) &= ~kDmaExpansionBus;  // completes immediately
                reg(kRegExpCtl) &= ~kExpCtlCpuHasBus;
                if (xbus_dma_) xbus_dma_();
                reg(kRegExpCtl) |= kExpCtlCpuHasBus;  // the CPU gets the bus back (Opera)
                raise_int0(kInt0ExpansionDmaDone);
            }
            return;
        case kRegDmaEnableClr:
            reg(kRegDmaEnableSet) &= ~value;
            update_dma_enable();
            return;
        case kRegExpCtl:
            // Opera ignores writes that turn DMA on: transfers complete at once
            // (through SetDMAEnable), so software waiting for XB_DMAON to clear
            // never sees it set.
            if (value & kExpCtlDmaOn) return;
            reg(offset) = value;
            return;
        default:
            if (is_timer_counter(offset)) {
                reg(offset) = value & 0xFFFF;
                return;
            }
            warn_unmodelled("write", offset);
            reg(offset) = value;
            return;
    }
    update_fiq();
}

void Clio::warn_unmodelled(const char* access, u32 offset) {
    if (!warned_offsets_.insert(offset).second) return;
    const std::string_view name = mmio_register_name(Memory::kMmioBase[3] + offset);
    Log::warn("CLIO: unmodelled register {} at +0x{:04X}{}{}", access, offset,
              name.empty() ? "" : " ", name);
}

}  // namespace core::clio
