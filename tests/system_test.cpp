// Tests log rate limiting and the runaway-CPU guard.
#include <vector>

#include "common/log.hpp"
#include "core/bus/memory.hpp"
#include "core/bus/mmio_names.hpp"
#include "core/system.hpp"
#include "test_common.hpp"

namespace {

void test_limiter_counts_everything() {
    Log::Limiter limiter("test", 3);
    for (int i = 0; i < 1000; ++i) limiter.warn("message {}", i);  // prints 3 + summary lines only
    CHECK_EQ(limiter.count(), 1000u);
    limiter.reset();
    CHECK_EQ(limiter.count(), 0u);
}

void test_unmapped_flood_is_counted() {
    core::Memory mem;
    for (u32 a = 0x04000000; a < 0x04000000 + 4 * 100000; a += 4) mem.read32(a);
    CHECK_EQ(mem.unmapped_access_count(), 100000u);
    CHECK_EQ(mem.read32(0x040B7240), 0u);
}

void test_executable_regions() {
    using core::Memory;
    CHECK_EQ(Memory::is_executable(0x00000000), true);
    CHECK_EQ(Memory::is_executable(0x002FFFFC), true);   // end of VRAM
    CHECK_EQ(Memory::is_executable(0x00300000), false);
    CHECK_EQ(Memory::is_executable(0x03000000), true);   // ROM
    CHECK_EQ(Memory::is_executable(0x03300000), false);  // MADAM
    CHECK_EQ(Memory::is_executable(0x040B7240), false);
}

void test_runaway_cpu_halts() {
    // "B ." now loops in place (branches are implemented); a jump into
    // unmapped space must halt the CPU instead of flooding the log.
    std::vector<u8> rom(core::Memory::kRomSize);
    for (std::size_t i = 0; i < rom.size(); i += 4) {
        rom[i] = 0xEA; rom[i + 1] = 0xFF; rom[i + 2] = 0xFF; rom[i + 3] = 0xFE;  // B .
    }
    {
        core::System looping;
        CHECK_EQ(looping.load_bios(rom), true);
        for (int f = 0; f < 3; ++f) looping.run_frame();
        CHECK_EQ(looping.cpu_halted(), false);
    }
    rom[0] = 0xE3; rom[1] = 0xA0; rom[2] = 0xF3; rom[3] = 0x01;  // MOV pc, #0x04000000
    core::System sys;
    CHECK_EQ(sys.load_bios(rom), true);
    for (int f = 0; f < 5 && !sys.cpu_halted(); ++f) sys.run_frame();
    CHECK_EQ(sys.cpu_halted(), true);

    const u64 frames = sys.frame_count();
    sys.run_frame();                      // keeps rendering while halted
    CHECK_EQ(sys.frame_count(), frames + 1);

    sys.reset();
    CHECK_EQ(sys.cpu_halted(), false);
}

void test_mmio_hit_counting() {
    core::Memory mem;
    u32 pc = 0x03001234;
    mem.set_pc_probe([&] { return pc; });

    for (int i = 0; i < 1000; ++i) mem.read32(0x03400034);  // a polling loop on CLIO VCnt
    mem.write32(0x03400040, 0x5);
    pc = 0x03005678;
    mem.write32(0x03400040, 0x7);

    const auto vcnt = mem.mmio_hits(0x03400034);
    CHECK_EQ(vcnt.has_value(), true);
    if (vcnt) {
        CHECK_EQ(vcnt->reads, 1000u);
        CHECK_EQ(vcnt->writes, 0u);
        CHECK_EQ(vcnt->last_pc, 0x03001234u);
        CHECK_EQ(vcnt->handled, false);
    }
    const auto int0 = mem.mmio_hits(0x03400040);
    CHECK_EQ(int0.has_value() && int0->writes == 2 && int0->last_value == 7 &&
                 int0->last_pc == 0x03005678u,
             true);
    mem.report_mmio_hotspots(3);
}

void test_fetch_is_not_a_register_read() {
    core::Memory mem;
    for (u32 a = 0x03100000; a < 0x03100400; a += 4) mem.fetch32(a);
    CHECK_EQ(mem.mmio_fetch_count(), 256u);
    CHECK_EQ(mem.mmio_hits(0x03100000).has_value(), false);  // not counted as polling
}

void test_register_names() {
    CHECK_EQ(core::mmio_register_name(0x03400034) == "CLIO VCnt", true);
    CHECK_EQ(core::mmio_register_name(0x03400000) == "CLIO ClioRev", true);
    CHECK_EQ(core::mmio_register_name(0x03400108) == "CLIO Timers", true);
    CHECK_EQ(core::mmio_register_name(0x03300008) == "MADAM MCTL", true);
    CHECK_EQ(core::mmio_register_name(0x03400090).empty(), true);
    CHECK_EQ(core::mmio_register_name(0x040B7240).empty(), true);
}

}  // namespace

int main() {
    test_mmio_hit_counting();
    test_fetch_is_not_a_register_read();
    test_register_names();
    test_limiter_counts_everything();
    test_unmapped_flood_is_counted();
    test_executable_regions();
    test_runaway_cpu_halts();
    return report("system_test");
}
