#include "core/system.hpp"

#include <algorithm>
#include <cmath>

#include "common/log.hpp"
#include "core/bus/memory.hpp"
#include "core/cpu/arm60.hpp"
#include "core/madam/cel.hpp"
#include "core/madam/madam.hpp"
#include "core/clio/clio.hpp"
#include "core/sport/sport.hpp"
#include "core/xbus/xbus.hpp"
#include "core/slowbus/slow_bus.hpp"
#include "core/video/vdlp.hpp"

namespace core {

System::System()
    : memory_(std::make_unique<Memory>()),
      cpu_(std::make_unique<cpu::Arm60>(*memory_)),
      madam_(std::make_unique<madam::Madam>(*memory_, player_bus_, memory_->ram())),
      // CLIO's interrupt output is wired to the ARM's FIQ input.
      clio_(std::make_unique<clio::Clio>([cpu = cpu_.get()](bool on) { cpu->set_fiq_line(on); })),
      sport_(std::make_unique<sport::Sport>(memory_->vram())),
      xbus_(std::make_unique<xbus::Xbus>(
          [clio = clio_.get()] { clio->raise_int0(clio::Clio::kInt0ExpansionBus); })),
      slow_bus_(std::make_unique<slowbus::SlowBus>()),
      vdlp_(std::make_unique<video::Vdlp>(memory_->vram())),
      cel_target_(static_cast<std::size_t>(kDemoWidth * kDemoHeight)),
      framebuffer_(384 * 288, 0xFF000000) {
    memory_->attach(MmioRegion::Madam, madam_.get());
    memory_->attach(MmioRegion::Clio, clio_.get());
    memory_->attach(MmioRegion::Sport, sport_.get());
    clio_->attach_xbus(xbus_.get());
    memory_->attach(MmioRegion::NvramDiag, slow_bus_.get());
    memory_->set_pc_probe([cpu = cpu_.get()] { return cpu->pc(); });
    // Player-bus DMA completion: CLIO Int1 bit 0 (INT1_PLYINT), as in Opera.
    madam_->set_player_dma_done_handler([clio = clio_.get()] { clio->raise_int1(1u << 0); });
    build_demo_cel();
    reset();
}

System::~System() = default;

u32 System::cpu_pc() const {
    return cpu_->pc();
}

void System::set_region(Region region) {
    region_ = region;
    Log::info("Region: {} ({} lines, {:.2f} Hz)", region == Region::Pal ? "PAL" : "NTSC",
              scanlines_per_field(), field_rate_hz());
}

double System::field_rate_hz() const {
    // Opera: OPERA_NTSC_FIELD_RATE_1616 = 3928227 / 65536, PAL = 50.
    return region_ == Region::Pal ? 50.0 : 3928227.0 / 65536.0;
}

int System::screen_width() const {
    if (!bios_loaded_) return kDemoWidth;
    return region_ == Region::Pal ? 384 : 320;
}

int System::screen_height() const {
    if (!bios_loaded_) return kDemoHeight;
    return region_ == Region::Pal ? 288 : 240;
}

// First displayed scanline of the field (Opera: NTSC 21, PAL 384-wide 22).
u32 System::first_visible_line() const {
    return region_ == Region::Pal ? 22 : 21;
}

u32 System::cycles_per_field() const {
    return static_cast<u32>(kCpuClockHz / field_rate_hz());
}

void System::reset() {
    memory_->reset();
    cpu_->reset();
    madam_->reset();
    clio_->reset();
    sport_->reset();
    xbus_->reset();
    odd_field_ = false;
    vdlp_->reset();
    std::ranges::fill(framebuffer_, 0xFF000000);
    frame_count_ = 0;
    total_cycles_ = 0;
    cycle_overshoot_ = 0;
    cpu_halted_ = false;
    Log::info("System reset ({} cycles/field)", cycles_per_field());
}

bool System::load_bios(std::span<const u8> image) {
    if (!memory_->load_rom(image)) return false;
    bios_loaded_ = true;
    reset();
    return true;
}

void System::run_frame() {
    // Without a BIOS there is nothing meaningful to execute.
    if (bios_loaded_ && !cpu_halted_) {
        // One field, scanline by scanline: CLIO's beam counter and vertical
        // interrupts advance with the CPU (Opera: opera_3do_internal_frame).
        const u32 lines = scanlines_per_field();
        const u64 field_cycles = cycles_per_field();
        for (u32 line = 0; line < lines; ++line) {
            clio_->set_beam(line, odd_field_);

            // Video: the VDLP scans out this line from VRAM.
            std::span<u32> out;
            const u32 first = first_visible_line();
            if (line >= first && line < first + static_cast<u32>(screen_height()))
                out = std::span(framebuffer_).subspan(
                    static_cast<std::size_t>((line - first) * static_cast<u32>(screen_width())),
                    static_cast<std::size_t>(screen_width()));
            const u32 mctl = madam_->mctl();
            vdlp_->process_line(line, madam_->vdl_head(), (mctl & madam::Madam::kMctlClutXen) != 0,
                                (mctl & madam::Madam::kMctlVscTxen) != 0, out);

            if (line == clio_->vint0_line()) clio_->raise_int0(clio::Clio::kInt0VInt0);
            if (line == clio_->vint1_line()) clio_->raise_int0(clio::Clio::kInt0VInt1);

            const auto line_cycles =
                static_cast<u32>(field_cycles * (line + 1) / lines - field_cycles * line / lines);
            const u32 budget = line_cycles - std::min(cycle_overshoot_, line_cycles);
            const u32 executed = budget > 0 ? cpu_->execute_cycles(budget) : 0;
            cycle_overshoot_ = cycle_overshoot_ + executed - line_cycles;
            total_cycles_ += executed;
            clio_->advance_timers(executed);
            xbus_->tick();
        }
        odd_field_ = !odd_field_;
        // TODO: catch up MADAM/timers/DSP to the CPU once they exist.

        // Real software never executes from MMIO or unmapped space; getting
        // there means the emulation went wrong (typically an unimplemented
        // branch executed as a NOP, letting the PC run linearly off the ROM).
        if (!Memory::is_executable(cpu_->pc())) {
            cpu_halted_ = true;
            Log::error("CPU halted: PC 0x{:08X} is outside RAM/VRAM/ROM after {} frames "
                       "({} unmapped accesses). Check the unimplemented-instruction "
                       "warnings above.",
                       cpu_->pc(), frame_count_ + 1, memory_->unmapped_access_count());
            memory_->report_mmio_hotspots(10);
        } else if (mmio_report_interval_ != 0 && (frame_count_ + 1) % mmio_report_interval_ == 0) {
            memory_->report_mmio_hotspots(10);
        }
    }

    const input::PadState pad = player_bus_.pad(0);
    demo_x_ += (pad.pressed(input::PadButton::Right) ? 2.0 : 0.0) -
               (pad.pressed(input::PadButton::Left) ? 2.0 : 0.0);
    demo_y_ += (pad.pressed(input::PadButton::Down) ? 2.0 : 0.0) -
               (pad.pressed(input::PadButton::Up) ? 2.0 : 0.0);

    if (!bios_loaded_) render_test_pattern();
    ++frame_count_;
}

void System::render_test_pattern() {
    // Gradient background in RGB555, drawn directly.
    const auto offset = static_cast<u32>(frame_count_);
    for (int y = 0; y < kDemoHeight; ++y) {
        for (int x = 0; x < kDemoWidth; ++x) {
            const u32 r = ((static_cast<u32>(x) + offset) >> 3) & 0x1F;
            const u32 g = ((static_cast<u32>(y) + offset) >> 3) & 0x1F;
            cel_target_[static_cast<std::size_t>(y * kDemoWidth + x)] =
                static_cast<u16>((r << 10) | (g << 5) | 0x10);
        }
    }

    // A rotating, breathing, warped cel on top. Its checker squares alternate
    // between opaque (P = 0, normal PIXC half) and blended (P = 1, average),
    // with transparent (black) holes on the diagonal.
    const double angle = static_cast<double>(frame_count_) * 0.02;
    const double scale = 7.0 + 2.0 * std::sin(angle * 1.7);
    const double hx = std::cos(angle) * scale;
    const double hy = std::sin(angle) * scale;
    const double warp = 0.25 * std::sin(angle * 0.9);
    const double half = kDemoCelSize / 2.0;

    madam::Ccb ccb;
    ccb.hdx = static_cast<s32>(hx * (1 << 20));
    ccb.hdy = static_cast<s32>(hy * (1 << 20));
    ccb.vdx = static_cast<s32>(-hy * (1 << 16));
    ccb.vdy = static_cast<s32>(hx * (1 << 16));
    ccb.hddx = static_cast<s32>(warp * hx / kDemoCelSize * (1 << 20));
    ccb.hddy = static_cast<s32>(warp * hy / kDemoCelSize * (1 << 20));
    const double cx = kDemoWidth / 2.0 + demo_x_;
    const double cy = kDemoHeight / 2.0 + demo_y_;
    ccb.x_pos = static_cast<s32>((cx - half * hx + half * hy) * (1 << 16));
    ccb.y_pos = static_cast<s32>((cy - half * hy - half * hx) * (1 << 16));
    ccb.pixc = (u32{madam::kPpmpAverage} << 16) | madam::kPpmpNormal;
    ccb.pre0 = ((kDemoCelSize - 1) << madam::pre0::kVCountShift) | madam::pre0::kLinear |
               madam::pre0::kBpp16;
    ccb.pre1 = ((kDemoCelSize * 2 / 4 - 2) << madam::pre1::kWOffset10Shift) |
               (1u << madam::pre1::kTlLsbShift) | (kDemoCelSize - 1);
    ccb.source = demo_cel_;
    madam::draw_cel(ccb, {cel_target_, kDemoWidth, kDemoHeight});

    // RGB555 -> ARGB8888 for the frontend (5-bit channels widened to 8 bits).
    for (std::size_t i = 0; i < cel_target_.size(); ++i) {
        const u32 p = cel_target_[i];
        const u32 r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
        framebuffer_[i] = 0xFF000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) |
                          ((b << 3) | (b >> 2));
    }
}

void System::build_demo_cel() {
    // 16 bpp literal pixels, big-endian, one row = kDemoCelSize * 2 bytes.
    demo_cel_.assign(kDemoCelSize * kDemoCelSize * 2, 0);
    for (u32 y = 0; y < kDemoCelSize; ++y) {
        for (u32 x = 0; x < kDemoCelSize; ++x) {
            u16 pixel = 0;  // black = transparent
            if (x != y && x != kDemoCelSize - 1 - y) {
                const bool light = ((x / 4) + (y / 4)) % 2 == 0;
                pixel = light ? static_cast<u16>(0x7FFF)                       // white, opaque
                              : static_cast<u16>(0x8000 | (31u << 10) | 8u);  // red, P=1: averaged
            }
            const std::size_t at = (y * kDemoCelSize + x) * 2;
            demo_cel_[at] = static_cast<u8>(pixel >> 8);
            demo_cel_[at + 1] = static_cast<u8>(pixel);
        }
    }
}

}  // namespace core
