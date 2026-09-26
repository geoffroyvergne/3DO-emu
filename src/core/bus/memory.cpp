#include "core/bus/memory.hpp"

#include <algorithm>

#include "common/endian.hpp"
#include "common/log.hpp"
#include "core/bus/mmio_names.hpp"

namespace core {

namespace {

constexpr u32 kRamEnd = Memory::kVramBase + Memory::kVramSize;  // DRAM and VRAM are contiguous

}  // namespace

Memory::Memory() : ram_(kVramBase + kVramSize), rom_(kRomSize) {
    reset();
}

void Memory::reset() {
    std::ranges::fill(ram_, u8{0});
    rom_overlay_ = true;
    mmio_hits_.clear();
    abort_pending_ = false;
    mmio_log_.reset();
    mmio_fetch_log_.reset();
    unmapped_fetch_log_.reset();
    unmapped_read_log_.reset();
    unmapped_write_log_.reset();
    rom_write_log_.reset();
    mmio_byte_log_.reset();
}

bool Memory::load_rom(std::span<const u8> image) {
    if (image.size() > kRomSize) {
        Log::error("BIOS image is {} bytes, ROM window is only {} bytes", image.size(), kRomSize);
        return false;
    }
    if (image.size() != kRomSize)
        Log::warn("BIOS image is {} bytes, expected {}; padding with zeros", image.size(), kRomSize);

    std::ranges::fill(rom_, u8{0});
    std::ranges::copy(image, rom_.begin());
    return true;
}

void Memory::attach(MmioRegion region, MmioDevice* device) {
    devices_[static_cast<std::size_t>(region)] = device;
}

u32 Memory::read32(u32 address) {
    return read(address, Access::Read);
}

u32 Memory::fetch32(u32 address) {
    return read(address, Access::Fetch);
}

u32 Memory::read(u32 address, Access access) {
    address &= ~3u;

    if (address < kRamEnd) {
        if (rom_overlay_ && address < kRomSize) return load_be32(&rom_[address]);
        return load_be32(&ram_[address]);
    }
    if ((address >> 20) == 0x030) return load_be32(&rom_[address - kRomBase]);

    const bool fetch = access == Access::Fetch;
    MmioRegion region = MmioRegion::Count;
    switch (address >> 20) {
        case 0x031: region = MmioRegion::NvramDiag; break;
        case 0x032: region = MmioRegion::Sport; break;
        case 0x033: region = MmioRegion::Madam; break;
        case 0x034: region = MmioRegion::Clio; break;
        default: break;
    }

    if (region != MmioRegion::Count) {
        if (fetch) {
            // Code never runs from registers: this is a runaway PC, not a poll.
            mmio_fetch_log_.warn("Instruction fetch from MMIO at 0x{:08X} ({}): the CPU is "
                                 "executing outside code memory",
                                 address, kMmioName[static_cast<std::size_t>(region)]);
            return 0;
        }
        return mmio_read32(region, address);
    }

    // Nothing is known to live here (Opera maps nothing either); read as 0.
    if (fetch)
        unmapped_fetch_log_.warn("Instruction fetch from unmapped address 0x{:08X}", address);
    else
        unmapped_read_log_.warn("Unmapped read32 at 0x{:08X} by PC 0x{:08X}", address, current_pc());
    return 0;
}

void Memory::write32(u32 address, u32 value) {
    address &= ~3u;
    // Any write swaps the boot ROM overlay out for DRAM (behaviour as described
    // by Opera; the exact trigger on real hardware is unverified).
    rom_overlay_ = false;

    if (address < kRamEnd) {
        store_be32(&ram_[address], value);
        return;
    }

    switch (address >> 20) {
        case 0x030:
            rom_write_log_.warn("Ignored write32 to ROM at 0x{:08X} = 0x{:08X} by PC 0x{:08X}",
                                address, value, current_pc());
            return;
        case 0x031: return mmio_write32(MmioRegion::NvramDiag, address, value);
        case 0x032: return mmio_write32(MmioRegion::Sport, address, value);
        case 0x033: return mmio_write32(MmioRegion::Madam, address, value);
        case 0x034: return mmio_write32(MmioRegion::Clio, address, value);
        default: break;
    }

    unmapped_write_log_.warn("Unmapped write32 at 0x{:08X} = 0x{:08X} by PC 0x{:08X}", address,
                             value, current_pc());
}

u8 Memory::read8(u32 address) {
    if (address < kRamEnd) {
        if (rom_overlay_ && address < kRomSize) return rom_[address];
        return ram_[address];
    }
    if ((address >> 20) == 0x030) return rom_[address - kRomBase];

    // MMIO and unmapped space: take the byte lane of the word access
    // (big-endian: byte 0 is bits 31-24). Unverified for real registers.
    if (address >= kMmioBase.front() && address < kMmioBase.back() + kMmioWindowSize)
        mmio_byte_log_.warn("Byte read from MMIO at 0x{:08X} by PC 0x{:08X}", address, current_pc());
    const u32 word = read(address & ~3u, Access::Read);
    return static_cast<u8>(word >> (24 - 8 * (address & 3)));
}

void Memory::write8(u32 address, u8 value) {
    rom_overlay_ = false;

    if (address < kRamEnd) {
        ram_[address] = value;
        return;
    }
    if ((address >> 20) == 0x030) {
        rom_write_log_.warn("Ignored write8 to ROM at 0x{:08X} = 0x{:02X} by PC 0x{:08X}", address,
                            value, current_pc());
        return;
    }

    // Opera treats MMIO byte writes (NVRAM) as a store of the byte into the
    // containing word; do the same until a device needs better.
    if (address >= kMmioBase.front() && address < kMmioBase.back() + kMmioWindowSize)
        mmio_byte_log_.warn("Byte write to MMIO at 0x{:08X} = 0x{:02X} by PC 0x{:08X}", address,
                            value, current_pc());
    write32(address & ~3u, value);
}

u32 Memory::mmio_read32(MmioRegion region, u32 address) {
    const auto index = static_cast<std::size_t>(region);
    MmioDevice* device = devices_[index];
    if (device && device->access_aborts(address - kMmioBase[index])) {
        abort_pending_ = true;
        return 0;
    }
    const u32 value = device ? device->mmio_read32(address - kMmioBase[index]) : 0;
    record_mmio(address, false, value, device != nullptr);
    return value;
}

void Memory::mmio_write32(MmioRegion region, u32 address, u32 value) {
    const auto index = static_cast<std::size_t>(region);
    MmioDevice* device = devices_[index];
    if (device && device->access_aborts(address - kMmioBase[index])) {
        abort_pending_ = true;
        return;
    }
    if (device) device->mmio_write32(address - kMmioBase[index], value);
    record_mmio(address, true, value, device != nullptr);
}

void Memory::record_mmio(u32 address, bool write, u32 value, bool handled) {
    MmioHits& hits = mmio_hits_[address];
    const u64 count = write ? ++hits.writes : ++hits.reads;
    hits.last_pc = current_pc();
    hits.last_value = value;
    hits.handled = handled;

    // Stubbed windows: report each new (address, direction) exactly once.
    if (handled || count != 1) return;
    const std::string_view name = mmio_register_name(address);
    mmio_log_.warn("Unhandled MMIO {} 0x{:08X}{}{} {} 0x{:08X} by PC 0x{:08X}",
                   write ? "write" : "read", address, name.empty() ? "" : " ", name,
                   write ? "<-" : "->", value, hits.last_pc);
}

std::optional<Memory::MmioHits> Memory::mmio_hits(u32 address) const {
    const auto it = mmio_hits_.find(address & ~3u);
    if (it == mmio_hits_.end()) return std::nullopt;
    return it->second;
}

void Memory::report_mmio_hotspots(std::size_t top_n) const {
    if (mmio_hits_.empty()) {
        Log::info("MMIO hotspots: no MMIO data accesses so far");
        return;
    }
    std::vector<std::pair<u32, MmioHits>> ranked(mmio_hits_.begin(), mmio_hits_.end());
    const std::size_t shown = std::min(top_n, ranked.size());
    std::ranges::partial_sort(ranked, ranked.begin() + static_cast<std::ptrdiff_t>(shown),
                              std::ranges::greater{}, [](const auto& entry) {
                                  return entry.second.reads + entry.second.writes;
                              });

    Log::info("MMIO hotspots (top {} of {} registers):", shown, ranked.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& [address, hits] = ranked[i];
        const std::string_view name = mmio_register_name(address);
        Log::info("  0x{:08X} {:<26} reads {:>10} writes {:>10}  last PC 0x{:08X} value 0x{:08X}{}",
                  address, name.empty() ? "?" : name, hits.reads, hits.writes, hits.last_pc,
                  hits.last_value, hits.handled ? "" : "  [stub]");
    }
}

}  // namespace core
