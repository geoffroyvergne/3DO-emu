#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <memory>
#include <span>
#include <vector>

#include "common/types.hpp"
#include "core/input/pbus.hpp"

namespace core {

class Memory;
namespace cpu {
class Arm60;
}
namespace madam {
class Madam;
}
namespace clio {
class Clio;
}
namespace sport {
class Sport;
}
namespace xbus {
class Xbus;
}
namespace slowbus {
class SlowBus;
}
namespace video {
class Vdlp;
}
namespace cdrom {
class DiscImage;
}
namespace dsp {
class AudioDma;
class Dspp;
}

// Video standard: sets the field rate and scanline count (values from Opera).
enum class Region { Ntsc, Pal };

// Top-level 3DO machine. Owns every hardware component (bus, ARM60, MADAM, CLIO...)
// and advances them in lockstep. Frontend-agnostic: it only exposes a framebuffer.
class System {
public:
    static constexpr u32 kCpuClockHz = 12'500'000;  // ARM60 @ 12.5 MHz

    // Forces the video standard. Without a call, load_bios() picks it from
    // the BIOS image (a PAL BIOS means a PAL console).
    void set_region(Region region);
    [[nodiscard]] Region region() const { return region_; }
    [[nodiscard]] double field_rate_hz() const;  // 59.94 (NTSC) or 50 (PAL)
    [[nodiscard]] u32 scanlines_per_field() const { return region_ == Region::Pal ? 312 : 263; }
    [[nodiscard]] u32 cycles_per_field() const;

    // Size of the no-BIOS demo image.
    static constexpr int kDemoWidth = 320;
    static constexpr int kDemoHeight = 240;

    System();
    ~System();

    void reset();

    [[nodiscard]] bool bios_loaded() const { return bios_loaded_; }

    // Loads a BIOS image and resets the machine. Returns false if rejected.
    bool load_bios(std::span<const u8> image);

    // Puts a disc image (.iso, .bin, .chd) in the CD drive. Returns false if it
    // cannot be opened. Takes effect for the next boot/reset.
    bool insert_disc(const std::filesystem::path& path);

    // Swaps discs on a running console, like opening the tray and closing it
    // on another disc about a second later. The OS then checks the new disc
    // (a multi-disc game picks it up; a different game reboots the console).
    // Returns false (keeping the current disc) if the image cannot be opened.
    bool change_disc(const std::filesystem::path& path);

    // Name of the disc in the drive (empty if none).
    [[nodiscard]] const std::string& disc_name() const { return disc_name_; }

    // Runs exactly one video frame worth of emulated cycles.
    void run_frame();

    // Output image in ARGB8888, row-major, screen_width() * screen_height()
    // pixels: the VDLP's output once a BIOS runs, the demo otherwise.
    [[nodiscard]] int screen_width() const;
    [[nodiscard]] int screen_height() const;
    [[nodiscard]] std::span<const u32> framebuffer() const {
        return std::span(framebuffer_).first(static_cast<std::size_t>(screen_width() * screen_height()));
    }

    [[nodiscard]] u64 frame_count() const { return frame_count_; }

    // Audio produced since the last call: interleaved stereo s16 at 44.1 kHz.
    [[nodiscard]] std::vector<s16> take_audio();
    [[nodiscard]] bool cpu_halted() const { return cpu_halted_; }
    [[nodiscard]] u32 cpu_pc() const;  // diagnostics

    // Direct component access for debugging tools and tests.
    [[nodiscard]] Memory& memory() { return *memory_; }
    [[nodiscard]] cpu::Arm60& cpu() { return *cpu_; }

    // Diagnostics: print the most accessed MMIO registers every `frames`
    // frames (0 = only when the CPU halts).
    void set_mmio_report_interval(u32 frames) { mmio_report_interval_ = frames; }

    // Control pads on the PBUS daisy chain; the frontend updates them before each frame.
    [[nodiscard]] input::PlayerBus& player_bus() { return player_bus_; }

private:
    // Placeholder video until MADAM/CLIO exist: background + one cel via the cel engine.
    void render_test_pattern();
    void build_demo_cel();

    static constexpr u32 kDemoCelSize = 16;

    std::unique_ptr<Memory> memory_;
    std::unique_ptr<cpu::Arm60> cpu_;
    input::PlayerBus player_bus_;
    std::unique_ptr<madam::Madam> madam_;
    std::unique_ptr<clio::Clio> clio_;
    std::unique_ptr<sport::Sport> sport_;
    std::unique_ptr<xbus::Xbus> xbus_;
    std::unique_ptr<slowbus::SlowBus> slow_bus_;
    std::unique_ptr<video::Vdlp> vdlp_;
    std::unique_ptr<dsp::AudioDma> audio_dma_;
    std::unique_ptr<dsp::Dspp> dspp_;
    u64 audio_phase_ = 0;          // CPU cycles * 44100, modulo the CPU clock
    std::vector<s16> audio_out_;
    u64 dsp_interrupts_ = 0;  // diagnostics
    std::unique_ptr<cdrom::DiscImage> pending_disc_;  // waiting for the tray to close
    u64 tray_close_frame_ = 0;
    std::string disc_name_;
    Region region_ = Region::Ntsc;
    bool region_forced_ = false;
    bool odd_field_ = false;
    bool bios_loaded_ = false;
    bool cpu_halted_ = false;  // set when the PC leaves executable memory
    u32 mmio_report_interval_ = 0;

    std::vector<u16> cel_target_;  // RGB555 render target for the demo cel
    std::vector<u8> demo_cel_;
    double demo_x_ = 0.0;  // D-pad offset of the demo cel, proves input reaches the core
    double demo_y_ = 0.0;

    [[nodiscard]] u32 first_visible_line() const;
    std::vector<u32> framebuffer_;  // sized for the largest mode (PAL 384x288)
    u64 frame_count_ = 0;
    u64 total_cycles_ = 0;
    u32 cycle_overshoot_ = 0;  // cycles the CPU ran past its previous budget
};

}  // namespace core
