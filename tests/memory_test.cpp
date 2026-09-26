// Tests for the 3DO memory map: region routing, endianness, ROM overlay, MMIO dispatch.
#include <vector>

#include "core/bus/memory.hpp"
#include "test_common.hpp"

namespace {

using core::Memory;

// Records the last access so tests can check the offset the bus hands over.
class FakeDevice final : public core::MmioDevice {
public:
    u32 mmio_read32(u32 offset) override {
        last_offset = offset;
        return 0xC0DE0000 | offset;
    }
    void mmio_write32(u32 offset, u32 value) override {
        last_offset = offset;
        last_value = value;
    }

    u32 last_offset = 0xFFFFFFFF;
    u32 last_value = 0;
};

std::vector<u8> make_rom() {
    std::vector<u8> rom(Memory::kRomSize);
    rom[0] = 0xE3; rom[1] = 0xA0; rom[2] = 0x00; rom[3] = 0x01;  // reset vector word
    rom[Memory::kRomSize - 4] = 0xAA;
    rom[Memory::kRomSize - 1] = 0x55;
    return rom;
}

void test_big_endian_storage() {
    Memory mem;
    mem.write32(0x100, 0x11223344);
    CHECK_EQ(mem.dram()[0x100], 0x11u);
    CHECK_EQ(mem.dram()[0x103], 0x44u);
    CHECK_EQ(mem.read32(0x100), 0x11223344u);
    CHECK_EQ(mem.read32(0x102), 0x11223344u);  // forced word alignment
}

void test_dram_vram_boundary() {
    Memory mem;
    mem.write32(Memory::kVramBase - 4, 0xDEADBEEF);
    mem.write32(Memory::kVramBase, 0xCAFEF00D);
    mem.write32(Memory::kVramBase + Memory::kVramSize - 4, 0x12345678);
    CHECK_EQ(mem.read32(Memory::kVramBase - 4), 0xDEADBEEFu);
    CHECK_EQ(mem.dram()[Memory::kDramSize - 4], 0xDEu);
    CHECK_EQ(mem.vram()[0], 0xCAu);
    CHECK_EQ(mem.read32(Memory::kVramBase), 0xCAFEF00Du);
    CHECK_EQ(mem.read32(Memory::kVramBase + Memory::kVramSize - 4), 0x12345678u);
    CHECK_EQ(mem.read32(Memory::kVramBase + Memory::kVramSize), 0u);  // unmapped
}

void test_rom_and_boot_overlay() {
    Memory mem;
    CHECK_EQ(mem.load_rom(make_rom()), true);
    CHECK_EQ(mem.rom_overlay_active(), true);
    CHECK_EQ(mem.read32(0x00000000), 0xE3A00001u);           // ROM visible at reset vector
    CHECK_EQ(mem.read32(Memory::kRomBase), 0xE3A00001u);
    CHECK_EQ(mem.read32(Memory::kRomBase + Memory::kRomSize - 4), 0xAA000055u);

    mem.write32(0x1000, 0x1);                                // first write drops the overlay
    CHECK_EQ(mem.rom_overlay_active(), false);
    CHECK_EQ(mem.read32(0x00000000), 0u);                    // now DRAM
    CHECK_EQ(mem.read32(Memory::kRomBase), 0xE3A00001u);     // ROM still at its home

    mem.write32(Memory::kRomBase, 0xFFFFFFFF);               // ROM is read-only
    CHECK_EQ(mem.read32(Memory::kRomBase), 0xE3A00001u);

    mem.reset();
    CHECK_EQ(mem.rom_overlay_active(), true);
    CHECK_EQ(mem.read32(0x00000000), 0xE3A00001u);
}

void test_rom_size_validation() {
    Memory mem;
    CHECK_EQ(mem.load_rom(std::vector<u8>(Memory::kRomSize + 0x40000)), false);  // 1.25 MB
    CHECK_EQ(mem.load_rom(std::vector<u8>(0x80000)), true);                       // padded
}

void test_mmio_dispatch() {
    Memory mem;
    FakeDevice madam;
    FakeDevice clio;
    mem.attach(core::MmioRegion::Madam, &madam);
    mem.attach(core::MmioRegion::Clio, &clio);

    CHECK_EQ(mem.read32(0x03300010), 0xC0DE0010u);
    CHECK_EQ(madam.last_offset, 0x10u);

    mem.write32(0x03400034, 0x99);
    CHECK_EQ(clio.last_offset, 0x34u);
    CHECK_EQ(clio.last_value, 0x99u);
    CHECK_EQ(madam.last_offset, 0x10u);  // untouched

    CHECK_EQ(mem.read32(0x03200000), 0u);  // SPORT has no device: logging stub

    mem.attach(core::MmioRegion::Madam, nullptr);
    CHECK_EQ(mem.read32(0x03300010), 0u);
}

}  // namespace

int main() {
    test_big_endian_storage();
    test_dram_vram_boundary();
    test_rom_and_boot_overlay();
    test_rom_size_validation();
    test_mmio_dispatch();
    return report("memory_test");
}
