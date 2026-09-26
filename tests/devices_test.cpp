// Tests for CLIO, SPORT and MADAM register behaviour the BIOS depends on.
#include <vector>

#include "core/bus/memory.hpp"
#include "core/clio/clio.hpp"
#include "core/input/pbus.hpp"
#include "core/madam/cel.hpp"
#include "core/madam/madam.hpp"
#include "core/slowbus/slow_bus.hpp"
#include "core/sport/sport.hpp"
#include "core/video/vdlp.hpp"
#include "common/endian.hpp"
#include "core/xbus/xbus.hpp"
#include "core/cdrom/disc_image.hpp"
#include "core/dsp/audio_dma.hpp"
#include "core/dsp/dspp.hpp"
#include "test_common.hpp"

namespace {

using core::clio::Clio;
using core::madam::Madam;
using core::sport::Sport;

void test_clio_identity_and_reset_reason() {
    Clio clio(nullptr);
    CHECK_EQ(clio.mmio_read32(Clio::kRegRevision), Clio::kRevisionGreen);
    CHECK_EQ(clio.mmio_read32(Clio::kRegCStatBits), Clio::kResetReasonDipir);
    clio.mmio_write32(Clio::kRegCStatBits, 0x20);  // CLEAR_DIPIR
    CHECK_EQ(clio.mmio_read32(Clio::kRegCStatBits), 0u);
}

void test_clio_interrupts_drive_fiq() {
    bool fiq = false;
    Clio clio([&](bool on) { fiq = on; });
    CHECK_EQ(fiq, false);

    clio.raise_int0(Clio::kInt0VInt1);          // pending but not enabled
    CHECK_EQ(fiq, false);
    clio.mmio_write32(Clio::kRegInt0EnSet, Clio::kInt0VInt1);
    CHECK_EQ(fiq, true);
    CHECK_EQ(clio.mmio_read32(Clio::kRegInt0Clr) & Clio::kInt0VInt1, Clio::kInt0VInt1);  // mirror
    clio.mmio_write32(Clio::kRegInt0Clr, Clio::kInt0VInt1);
    CHECK_EQ(fiq, false);

    clio.raise_int1(1);                          // bank 1, not enabled: no summary bit
    CHECK_EQ(clio.mmio_read32(Clio::kRegInt0Set) & Clio::kInt0SecondBank, 0u);
    CHECK_EQ(fiq, false);
    clio.mmio_write32(Clio::kRegInt1EnSet, 1);   // enabled: Int0 bit 31 + FIQ
    CHECK_EQ(clio.mmio_read32(Clio::kRegInt0Set) & Clio::kInt0SecondBank, Clio::kInt0SecondBank);
    CHECK_EQ(fiq, true);
    clio.mmio_write32(Clio::kRegInt0Clr, Clio::kInt0SecondBank);  // derived: cannot be cleared
    CHECK_EQ(fiq, true);
    clio.mmio_write32(Clio::kRegInt1Clr, 1);
    CHECK_EQ(clio.mmio_read32(Clio::kRegInt0Set) & Clio::kInt0SecondBank, 0u);
    CHECK_EQ(fiq, false);
}

void test_clio_beam_counter() {
    Clio clio(nullptr);
    clio.set_beam(123, true);
    CHECK_EQ(clio.mmio_read32(Clio::kRegVCnt), 123u | 0x800u);
    clio.mmio_write32(Clio::kRegVInt0, 0xFFFF);
    CHECK_EQ(clio.vint0_line(), 0x7FFu);
}

void test_clio_timers() {
    bool fiq = false;
    Clio clio([&](bool on) { fiq = on; });
    clio.mmio_write32(Clio::kRegTimerFirst + 8 * 1, 3);      // timer 1 count
    clio.mmio_write32(Clio::kRegTimerFirst + 8 * 1 + 4, 5);  // timer 1 reload
    clio.mmio_write32(Clio::kRegTimerCtlLoSet, (0x1 | 0x2) << 4);  // timer 1: decrement + reload
    clio.mmio_write32(Clio::kRegInt0EnSet, 1u << 10);        // TIMINT1

    const u32 scan_cycles = (16 * 4 + 64) / 2;               // default slack 64
    clio.advance_timers(scan_cycles * 3);                    // 3 -> 0
    CHECK_EQ(clio.mmio_read32(Clio::kRegTimerFirst + 8), 0u);
    CHECK_EQ(fiq, false);
    clio.advance_timers(scan_cycles);                        // underflow: reload + interrupt
    CHECK_EQ(clio.mmio_read32(Clio::kRegTimerFirst + 8), 5u);
    CHECK_EQ(fiq, true);
    CHECK_EQ(clio.mmio_read32(Clio::kRegInt0Set) & (1u << 10), 1u << 10);

    // Cascade: timer 3 only counts when timer 2 underflows.
    clio.mmio_write32(Clio::kRegTimerFirst + 8 * 2, 0);
    clio.mmio_write32(Clio::kRegTimerFirst + 8 * 2 + 4, 1);
    clio.mmio_write32(Clio::kRegTimerFirst + 8 * 3, 10);
    clio.mmio_write32(Clio::kRegTimerCtlLoSet, ((0x1 | 0x2) << 8) | ((0x1 | 0x4) << 12));
    clio.advance_timers(scan_cycles * 4);                    // timer 2 underflows twice
    CHECK_EQ(clio.mmio_read32(Clio::kRegTimerFirst + 8 * 3), 8u);
}

void test_sport_copy_and_fill() {
    std::vector<u8> vram(1024 * 1024);
    Sport sport(vram);
    for (u32 i = 0; i < Sport::kPageBytes; ++i) vram[0x56800 + i] = static_cast<u8>(i);

    sport.mmio_read32(0x2B4);                    // source = page 0x2B4 -> 0x56800
    sport.mmio_write32(0x598, 0xFFFFFFFF);       // copy to page 0x598 -> 0xB3000
    CHECK_EQ(vram[0xB3000], 0u);
    CHECK_EQ(vram[0xB3000 + 0x7FF], 0xFFu);

    sport.mmio_write32(0x2000, 0x12345678);      // fill colour
    sport.mmio_write32(0x4000 | 0x10, 0xFFFFFFFF);  // flash-write page 0x10
    CHECK_EQ(vram[0x10 << 9], 0x12u);            // big-endian word
    CHECK_EQ(vram[(0x10 << 9) + Sport::kPageBytes - 1], 0x78u);
    CHECK_EQ(vram[(0x10 << 9) + Sport::kPageBytes], 0u);  // exactly one page
}

void test_xbus_cd_drive() {
    int interrupts = 0;
    core::xbus::Xbus xbus([&] { ++interrupts; });
    using core::xbus::Xbus;

    xbus.write(Xbus::kSelect, 0);                        // CD drive
    CHECK_EQ(xbus.access_aborts(Xbus::kPoll), false);
    for (u32 b : {0x83u, 0u, 0u, 0u, 0u, 0u, 0u}) xbus.write(Xbus::kFifoCommandStatus, b);  // READ ID
    CHECK_EQ(xbus.read(Xbus::kPoll) & 0x10, 0x10u);      // status valid
    CHECK_EQ(interrupts > 0, true);
    const u8 expected[] = {0x83, 0x00, 0x10, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0x83};  // ready, door, 2x
    for (u8 e : expected) CHECK_EQ(xbus.read(Xbus::kFifoCommandStatus), e);
    CHECK_EQ(xbus.read(Xbus::kPoll) & 0x10, 0u);         // drained

    for (u32 b : {0x8Cu, 0u, 0u, 0u, 0u, 0u, 0u}) xbus.write(Xbus::kFifoCommandStatus, b);  // READ TOC
    CHECK_EQ(xbus.read(Xbus::kFifoCommandStatus), 0x8Cu);
    CHECK_EQ(xbus.read(Xbus::kFifoCommandStatus), 0x92u);  // door closed + error + 2x, no disc

    xbus.write(Xbus::kSelect, 3);                        // nothing in slot 3
    CHECK_EQ(xbus.access_aborts(Xbus::kPoll), true);
    CHECK_EQ(xbus.access_aborts(Xbus::kSelect), false);  // select register never aborts
}

// A disc whose sector n is filled with byte n.
class FakeDisc final : public core::cdrom::DiscImage {
public:
    bool read_sector(u32 lba, std::span<u8, core::cdrom::kSectorSize> out) override {
        std::fill(out.begin(), out.end(), static_cast<u8>(lba));
        return true;
    }
    [[nodiscard]] u32 sector_count() const override { return 1000; }
    [[nodiscard]] Format format() const override { return Format::Cooked2048; }
};

void test_cd_drive_with_disc() {
    using core::xbus::CdDrive;
    CdDrive drive;
    drive.insert_disc(std::make_unique<FakeDisc>());
    auto command = [&](std::initializer_list<u8> bytes) {
        for (u8 b : bytes) drive.write_command(b);
    };
    auto status = [&](std::size_t n) {
        std::vector<u8> out;
        for (std::size_t i = 0; i < n; ++i) out.push_back(drive.read_status());
        return out;
    };

    command({0x8B, 0, 0, 0, 0, 0, 0});  // READ DISC INFO: 1000 sectors -> lead-out 00:15:25
    const auto info = status(8);
    CHECK_EQ(info[1], 0u);              // disc type CD-ROM
    CHECK_EQ(info[2], 1u);              // tracks 1..1
    CHECK_EQ(info[3], 1u);
    CHECK_EQ(info[4], 0u);
    CHECK_EQ(info[5], 15u);
    CHECK_EQ(info[6], 25u);
    CHECK_EQ(info[7], 0xE3u);           // ready, door, disc, spinning, 2x

    command({0x8C, 0, 1, 0, 0, 0, 0});  // READ TOC track 1: data track at 00:02:00
    const auto toc = status(10);
    CHECK_EQ(toc[2], 0x14u);
    CHECK_EQ(toc[6], 2u);

    command({0x10, 0, 2, 5, 0, 0, 2});  // READ DATA 00:02:05 (LBA 5), 2 blocks
    CHECK_EQ(drive.poll() & 0x30, 0u);  // nothing until the drive has read a sector
    drive.advance(CdDrive::kCpuClockHz / 150);
    CHECK_EQ(drive.poll() & 0x30, 0x30u);  // status + data valid together
    const auto read_status = status(2);
    CHECK_EQ(read_status[0], 0x10u);
    CHECK_EQ(drive.data_available(), 2048u);
    u32 sum = 0;
    for (int i = 0; i < 2048; ++i) sum += drive.read_data();
    CHECK_EQ(sum, 5u * 2048);
    CHECK_EQ(drive.poll() & 0x20, 0u);
    drive.advance(CdDrive::kCpuClockHz / 150);
    CHECK_EQ(drive.read_data(), 6u);    // second sector

    command({0x06, 0, 0, 0, 0, 0, 0});  // eject
    status(2);
    command({0x8B, 0, 0, 0, 0, 0, 0});
    CHECK_EQ(status(2)[1] & 0x10, 0x10u);  // error: disc out
}

void test_dsp_semaphore_program() {
    std::vector<u8> ram(0x1000);
    core::dsp::AudioDma fifos(ram, nullptr);
    core::dsp::Dspp dsp(fifos);

    // Idle DSP: code memory is all `sleep`, output silent.
    CHECK_EQ(dsp.run_sample(), 0u);

    // ARM writes the semaphore: status 8 ("ARM last").
    dsp.write_semaphore(0xBEEF);
    CHECK_EQ(dsp.read_semaphore(), 0x0008BEEFu);

    // MOVE #0x123 -> 0x3ED (semaphore data), MOVE #0x0456 -> 0x3FE (left DAC), sleep.
    const u16 program[] = {0x9BED, 0xC123, 0x9BFE, 0xC456, 0x8380};
    for (u32 i = 0; i < std::size(program); ++i) dsp.write_code(i, program[i]);
    dsp.set_running(true);
    const u32 out = dsp.run_sample();
    CHECK_EQ(dsp.read_semaphore(), 0x00040123u);  // status 4: DSP wrote last
    CHECK_EQ(out & 0xFFFF, 0x0456u);
}

void test_dsp_input_fifo() {
    std::vector<u8> ram(0x1000);
    u32 interrupts = 0;
    core::dsp::AudioDma fifos(ram, [&](u32 bits) { interrupts |= bits; });
    ram[0x100] = 0x12; ram[0x101] = 0x34; ram[0x102] = 0x56; ram[0x103] = 0x78;
    ram[0x200] = 0x9A; ram[0x201] = 0xBC;

    fifos.write_register(0x400 + 0x20 + 0x0, 0x100);  // channel 2: 8 bytes at 0x100
    fifos.write_register(0x400 + 0x20 + 0x4, 4);      // length written as bytes - 4
    fifos.write_register(0x400 + 0x20 + 0x8, 0x200);  // then 8 bytes at 0x200
    fifos.write_register(0x400 + 0x20 + 0xC, 4);
    fifos.set_dma_enable(1u << 2);

    CHECK_EQ(fifos.input_status(2), 2u);
    CHECK_EQ(fifos.pop_input(2), 0x1234u);
    CHECK_EQ(fifos.pop_input(2), 0x5678u);
    fifos.pop_input(2);
    fifos.pop_input(2);
    CHECK_EQ(interrupts, 0u);
    CHECK_EQ(fifos.pop_input(2), 0x9ABCu);          // drained: interrupt, reload
    CHECK_EQ(interrupts, 1u << (16 + 2));
    CHECK_EQ(fifos.read_register(0x420), 0x202u);   // current address advanced
}

void test_cd_tray_change() {
    using core::xbus::CdDrive;
    CdDrive drive;
    drive.insert_disc(std::make_unique<FakeDisc>());
    auto command = [&](std::initializer_list<u8> bytes) { for (u8 b : bytes) drive.write_command(b); };

    drive.open_tray();
    CHECK_EQ(drive.tray_open(), true);
    CHECK_EQ(drive.poll() & 0x80, 0x80u);          // media-changed latch
    command({0x8B, 0, 0, 0, 0, 0, 0});
    CHECK_EQ(drive.read_status(), 0x8Bu);
    CHECK_EQ(drive.read_status() & 0xC0, 0u);      // door open, no disc

    drive.write_poll(0x80);                        // software acknowledges the latch
    CHECK_EQ(drive.poll() & 0x80, 0u);
    drive.close_tray(std::make_unique<FakeDisc>());
    CHECK_EQ(drive.tray_open(), false);
    CHECK_EQ(drive.poll() & 0x80, 0x80u);
    command({0x8B, 0, 0, 0, 0, 0, 0});             // the new disc answers
    CHECK_EQ(drive.read_status(), 0x8Bu);
    CHECK_EQ(drive.read_status(), 0u);             // disc type CD-ROM
}

void test_nvram() {
    core::slowbus::SlowBus slow;
    slow.mmio_write32(0x40000 + 4 * 10, 0x1234AB);       // one byte per word
    CHECK_EQ(slow.mmio_read32(0x40000 + 4 * 10), 0xABu);
    CHECK_EQ(slow.nvram()[10], 0xABu);
    CHECK_EQ(slow.mmio_read32(0x80000), 0u);             // diagnostic port
}

void test_madam_cel_list() {
    core::Memory memory;
    core::input::PlayerBus pbus;
    Madam madam(memory, pbus, memory.ram());
    auto ram = memory.ram();
    auto put = [&](u32 address, u32 value) { store_be32(&ram[address], value); };

    using namespace core::madam;
    const u32 flags = ccb_flag::kLast | ccb_flag::kNpAbs | ccb_flag::kSpAbs | ccb_flag::kPpAbs |
                      ccb_flag::kYoxy | ccb_flag::kLdSize | ccb_flag::kLdPpmp | ccb_flag::kCcbPre |
                      ccb_flag::kAcw | ccb_flag::kAccw;
    const u32 words[] = {flags, 0, 0x2000, 0,        // flags, next, source, PLUT
                         2u << 16, 1u << 16,         // X, Y
                         1u << 20, 0, 0, 1u << 16,   // HDX HDY VDX VDY
                         (u32{kPpmpNormal} << 16) | kPpmpNormal,
                         pre0::kBpp16 | pre0::kLinear,          // 1 row
                         (1u << pre1::kTlLsbShift) | 0u};       // 1 pixel, stride 8 bytes
    for (u32 i = 0; i < std::size(words); ++i) put(0x1000 + 4 * i, words[i]);
    put(0x2000, 0x7C000000);  // one red pixel

    const u32 base = 0x03300000;
    memory.attach(core::MmioRegion::Madam, &madam);
    memory.write32(base + Madam::kRegRegCtl0, 0x1414);      // 320-pixel modulo, read and write
    memory.write32(base + Madam::kRegRegCtl1, 0x00EF013F);  // clip 319 x 239
    memory.write32(base + Madam::kRegRegCtl2, 0x200000);
    memory.write32(base + Madam::kRegRegCtl3, 0x200000);
    memory.write32(base + Madam::kRegNextCcb, 0x1000);
    memory.write32(base + Madam::kRegSprStart, 0);

    CHECK_EQ(madam.cels_drawn(), 1u);
    const std::size_t at = 0x200000 + 2 + 2 * 4;  // (2,1): pair 0, odd half, x = 2
    CHECK_EQ((ram[at] << 8) | ram[at + 1], 0x7C00u);
    CHECK_EQ(memory.read32(base + Madam::kRegStatBits), 0u);
}

void test_vdlp_scanout() {
    std::vector<u8> vram(1024 * 1024);
    auto put = [&](u32 address, u32 value) { store_be32(&vram[address], value); };
    // One entry at 0x1000: 2 optional words, current FBA override, bitmap DMA on,
    // 320-pixel modulo, persisting 240 lines; its next pointer loops to itself.
    const u32 control = 240 | (2u << 9) | (1u << 16) | (1u << 21);
    put(0x1000, control);
    put(0x1004, 0x10000);                       // frame buffer
    put(0x1008, 0);
    put(0x100C, 0x1000);
    put(0x1010, (1u << 24) | 0x112233);         // CLUT[1] = (0x11, 0x22, 0x33)
    put(0x1014, 0xE0000000 | 0x445566);         // background colour
    put(0x10000, (0x0421u << 16) | 0x0421u);    // (0,0) and (0,1): all channels index 1

    core::video::Vdlp vdlp(vram);
    std::vector<u32> line(320);
    for (u32 l = 0; l < 5; ++l) vdlp.process_line(l, 0x1000, true, true, {});
    vdlp.process_line(5, 0x1000, true, true, line);
    CHECK_EQ(line[0], 0xFF112233u);
    CHECK_EQ(line[1], 0xFF445566u);             // pixel 0 shows the background
    vdlp.process_line(6, 0x1000, true, true, line);
    CHECK_EQ(line[0], 0xFF112233u);             // odd line: other half of the word
    vdlp.process_line(7, 0x1000, true, false, line);
    CHECK_EQ(line[0], 0xFF000000u);             // video DMA off: black
}

void test_madam_identity() {
    core::Memory memory;
    core::input::PlayerBus pbus;
    Madam madam(memory, pbus);
    CHECK_EQ(madam.mmio_read32(Madam::kRegRevision), Madam::kRevisionGreen);
    CHECK_EQ(madam.mmio_read32(Madam::kRegMSysBits), Madam::kMSysBits2MbDram1MbVram);
    madam.mmio_write32(Madam::kRegMSysBits, 0x22);  // read-only
    CHECK_EQ(madam.mmio_read32(Madam::kRegMSysBits), Madam::kMSysBits2MbDram1MbVram);
}

}  // namespace

int main() {
    test_clio_identity_and_reset_reason();
    test_clio_interrupts_drive_fiq();
    test_clio_beam_counter();
    test_clio_timers();
    test_sport_copy_and_fill();
    test_madam_identity();
    test_xbus_cd_drive();
    test_madam_cel_list();
    test_vdlp_scanout();
    test_nvram();
    test_cd_drive_with_disc();
    test_cd_tray_change();
    test_dsp_semaphore_program();
    test_dsp_input_fifo();
    return report("devices_test");
}
