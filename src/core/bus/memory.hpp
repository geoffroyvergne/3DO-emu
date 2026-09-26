#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/log.hpp"
#include "common/types.hpp"
#include "core/bus/bus.hpp"

namespace core {

// A hardware block exposing registers through a 1 MB MMIO window.
// `offset` is relative to the window base.
class MmioDevice {
public:
    virtual ~MmioDevice() = default;

    virtual u32 mmio_read32(u32 offset) = 0;
    virtual void mmio_write32(u32 offset, u32 value) = 0;

    // Whether an access to `offset` right now must abort instead of reaching
    // the register (e.g. an absent expansion-bus device).
    [[nodiscard]] virtual bool access_aborts(u32 /*offset*/) const { return false; }
};

// Window layout cross-checked against the Opera (libretro) emulator source;
// not yet confirmed against official 3DO documentation.
enum class MmioRegion : std::size_t {
    NvramDiag,  // 0x03100000: NVRAM (32 KB) at +0x40000, diagnostic port at +0x80000
    Sport,      // 0x03200000: VRAM serial port (SPORT) transfers
    Madam,      // 0x03300000: MADAM (cel engine, DMA, math)
    Clio,       // 0x03400000: CLIO (video, timers, interrupts, DSP, XBUS)
    Count,
};

// The 3DO system memory map.
//
//   0x00000000-0x001FFFFF  DRAM (2 MB)
//   0x00200000-0x002FFFFF  VRAM (1 MB)
//   0x03000000-0x030FFFFF  BIOS ROM (1 MB)
//   0x03100000-0x034FFFFF  MMIO windows (see MmioRegion)
//
// At reset the ROM also appears at 0x00000000 until the first write, so the
// ARM60 can boot from its reset vector.
class Memory final : public Bus {
public:
    static constexpr u32 kDramBase = 0x00000000;
    static constexpr u32 kDramSize = 2 * 1024 * 1024;
    static constexpr u32 kVramBase = 0x00200000;
    static constexpr u32 kVramSize = 1 * 1024 * 1024;
    static constexpr u32 kRomBase = 0x03000000;
    static constexpr u32 kRomSize = 1 * 1024 * 1024;
    static constexpr u32 kMmioWindowSize = 0x00100000;

    static constexpr std::size_t kMmioRegionCount = static_cast<std::size_t>(MmioRegion::Count);
    static constexpr std::array<u32, kMmioRegionCount> kMmioBase = {
        0x03100000, 0x03200000, 0x03300000, 0x03400000,
    };
    static constexpr std::array<std::string_view, kMmioRegionCount> kMmioName = {
        "NVRAM/Diag", "SPORT", "MADAM", "CLIO",
    };

    Memory();

    // Clears DRAM/VRAM and re-enables the boot ROM overlay. ROM contents are kept.
    void reset();

    // Copies a BIOS image into ROM. Returns false if it does not fit.
    bool load_rom(std::span<const u8> image);

    // Routes a MMIO window to a device (nullptr restores the logging stub).
    void attach(MmioRegion region, MmioDevice* device);

    // Word accesses. Addresses are forced to word alignment; the ARM's rotation
    // of unaligned LDR results is the CPU's job.
    u32 read32(u32 address) override;
    void write32(u32 address, u32 value) override;
    u32 fetch32(u32 address) override;
    u8 read8(u32 address) override;
    void write8(u32 address, u8 value) override;
    bool consume_abort() override { return std::exchange(abort_pending_, false); }

    // Raw big-endian storage, for DMA and video scan-out.
    // DRAM and VRAM form one contiguous block (0x000000-0x2FFFFF).
    [[nodiscard]] std::span<u8> ram() { return ram_; }
    [[nodiscard]] std::span<u8> dram() { return std::span(ram_).first(kDramSize); }
    [[nodiscard]] std::span<u8> vram() { return std::span(ram_).subspan(kVramBase, kVramSize); }

    [[nodiscard]] bool rom_overlay_active() const { return rom_overlay_; }

    // True for regions that hold code: DRAM, VRAM and ROM. The system uses this
    // to stop a runaway CPU instead of fetching from MMIO or unmapped space.
    [[nodiscard]] static bool is_executable(u32 address) {
        return address < kVramBase + kVramSize ||
               (address >= kRomBase && address < kRomBase + kRomSize);
    }

    // Total accesses that hit nothing, for diagnostics.
    [[nodiscard]] u64 unmapped_access_count() const {
        return unmapped_read_log_.count() + unmapped_write_log_.count() + unmapped_fetch_log_.count();
    }

    // --- MMIO diagnostics -------------------------------------------------
    // Every MMIO data access is counted per address (handled or not), so a
    // polling loop shows up as one register with a huge count.
    struct MmioHits {
        u64 reads = 0;
        u64 writes = 0;
        u32 last_pc = 0;     // instruction that made the latest access
        u32 last_value = 0;  // value read or written by it
        bool handled = false;
    };

    // Supplies the current instruction address for log lines (optional).
    void set_pc_probe(std::function<u32()> probe) { pc_probe_ = std::move(probe); }

    [[nodiscard]] std::optional<MmioHits> mmio_hits(u32 address) const;
    [[nodiscard]] u64 mmio_fetch_count() const { return mmio_fetch_log_.count(); }

    // Logs the `top_n` most accessed MMIO registers.
    void report_mmio_hotspots(std::size_t top_n) const;

private:
    enum class Access { Read, Fetch };

    u32 read(u32 address, Access access);
    u32 mmio_read32(MmioRegion region, u32 address);
    void mmio_write32(MmioRegion region, u32 address, u32 value);
    void record_mmio(u32 address, bool write, u32 value, bool handled);
    [[nodiscard]] u32 current_pc() const { return pc_probe_ ? pc_probe_() : 0; }

    std::vector<u8> ram_;  // DRAM followed by VRAM
    std::vector<u8> rom_;
    std::array<MmioDevice*, kMmioRegionCount> devices_{};
    bool rom_overlay_ = true;
    bool abort_pending_ = false;
    std::function<u32()> pc_probe_;
    // Bounded: at most one entry per word of the four 1 MB MMIO windows.
    std::unordered_map<u32, MmioHits> mmio_hits_;
    // One line per newly seen (address, direction); the limiter caps sweeps.
    Log::Limiter mmio_log_{"Unhandled MMIO access", 64};
    Log::Limiter mmio_fetch_log_{"Instruction fetch from MMIO", 4};
    Log::Limiter unmapped_read_log_{"Unmapped read32"};
    Log::Limiter unmapped_write_log_{"Unmapped write32"};
    Log::Limiter unmapped_fetch_log_{"Instruction fetch from unmapped memory", 4};
    Log::Limiter rom_write_log_{"ROM write"};
    Log::Limiter mmio_byte_log_{"Byte access to MMIO", 8};
};

}  // namespace core
