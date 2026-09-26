#include <SDL.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/log.hpp"
#include "common/types.hpp"
#include "core/bus/memory.hpp"
#include "core/cdrom/disc_image.hpp"
#include "core/system.hpp"
#include "frontend/sdl_input.hpp"

namespace {

constexpr int kWindowScale = 3;

struct SdlContext {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioDeviceID audio = 0;

    ~SdlContext() {
        if (audio) SDL_CloseAudioDevice(audio);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

bool init_sdl(SdlContext& sdl, int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        Log::error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    sdl.window = SDL_CreateWindow("3DO Emu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  width * kWindowScale, height * kWindowScale,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!sdl.window) {
        Log::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    // No PRESENTVSYNC: pacing is driven by our own 60 Hz timer, so the emulation
    // speed stays correct on 120/144 Hz displays.
    sdl.renderer = SDL_CreateRenderer(sdl.window, -1, SDL_RENDERER_ACCELERATED);
    if (!sdl.renderer) {
        Log::error("SDL_CreateRenderer failed: {}", SDL_GetError());
        return false;
    }
    SDL_RenderSetLogicalSize(sdl.renderer, width, height);

    sdl.texture = SDL_CreateTexture(sdl.renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!sdl.texture) {
        Log::error("SDL_CreateTexture failed: {}", SDL_GetError());
        return false;
    }

    // 44.1 kHz stereo s16, the DSP's native output. Audio is optional.
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    sdl.audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (sdl.audio)
        SDL_PauseAudioDevice(sdl.audio, 0);
    else
        Log::warn("No audio output: {}", SDL_GetError());
    return true;
}

// Queues the frame's samples; drops audio rather than letting latency grow
// when the queue is already a quarter of a second deep.
void play_audio(SdlContext& sdl, core::System& system) {
    const std::vector<s16> samples = system.take_audio();
    if (!sdl.audio || samples.empty()) return;
    constexpr Uint32 kMaxQueuedBytes = 44100 * 2 * sizeof(s16) / 4;
    if (SDL_GetQueuedAudioSize(sdl.audio) > kMaxQueuedBytes) return;
    SDL_QueueAudio(sdl.audio, samples.data(), static_cast<Uint32>(samples.size() * sizeof(s16)));
}

std::optional<std::vector<u8>> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Title of a disc image without its "(Disc N)" / "(CD N)" suffix, so the
// discs of one game compare equal: "Wing Commander III (USA) (Disc 2)" ->
// "wing commander iii (usa)".
std::string game_title(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::regex kDiscSuffix(R"(\s*[\(\[](disc|disk|cd)\s*\d+[\)\]])");
    name = std::regex_replace(name, kDiscSuffix, "");
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    return name;
}

void update_title(SdlContext& sdl, const core::System& system) {
    const std::string title = system.disc_name().empty() ? "3DO Emu" : "3DO Emu - " + system.disc_name();
    SDL_SetWindowTitle(sdl.window, title.c_str());
}

// A file dropped on the window:
//   - a BIOS image (1 MB, not a disc): load it and boot;
//   - another disc of the game in the drive: swap it like a real tray change,
//     so multi-disc games can pick it up (Ctrl forces this);
//   - any other disc: insert it and reboot (Shift forces this).
void handle_drop(const char* path, core::System& system) {
    const std::filesystem::path file(path);
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    auto probe = core::cdrom::DiscImage::open(file);
    if (!probe && !ec && size <= core::Memory::kRomSize) {
        if (const auto bios = read_file(path); bios && system.load_bios(*bios))
            Log::info("Loaded BIOS '{}'", file.filename().string());
        return;
    }
    if (!probe) {
        Log::error("'{}' is neither a disc image nor a BIOS", file.filename().string());
        return;
    }
    probe.reset();

    const SDL_Keymod mods = SDL_GetModState();
    const bool same_game = !system.disc_name().empty() &&
                           game_title(system.disc_name()) == game_title(file.stem().string());
    const bool hot_swap = (mods & KMOD_CTRL) || (same_game && !(mods & KMOD_SHIFT));
    if (hot_swap && system.bios_loaded()) {
        system.change_disc(file);
    } else if (system.insert_disc(file)) {
        if (system.bios_loaded()) {
            Log::info("Booting '{}'", file.stem().string());
            system.reset();
        } else {
            Log::info("Disc inserted; drop a BIOS image to boot");
        }
    }
}

// Returns false when the user asked to quit.
bool handle_events(frontend::SdlInput& input, core::System& system, SdlContext& sdl) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        input.handle_event(event);
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) return false;
                if (event.key.keysym.sym == SDLK_F5 && !event.key.repeat) {
                    Log::info("Reset");
                    system.reset();
                }
                break;
            case SDL_DROPFILE:
                handle_drop(event.drop.file, system);
                SDL_free(event.drop.file);
                update_title(sdl, system);
                break;
            default:
                break;
        }
    }
    return true;
}

void render(SdlContext& sdl, const core::System& system) {
    // The output size follows the region, which a newly dropped BIOS can change.
    int tex_w = 0, tex_h = 0;
    SDL_QueryTexture(sdl.texture, nullptr, nullptr, &tex_w, &tex_h);
    if (tex_w != system.screen_width() || tex_h != system.screen_height()) {
        SDL_DestroyTexture(sdl.texture);
        sdl.texture = SDL_CreateTexture(sdl.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        system.screen_width(), system.screen_height());
        SDL_RenderSetLogicalSize(sdl.renderer, system.screen_width(), system.screen_height());
    }
    const auto fb = system.framebuffer();
    SDL_UpdateTexture(sdl.texture, nullptr, fb.data(),
                      system.screen_width() * static_cast<int>(sizeof(u32)));
    SDL_RenderClear(sdl.renderer);
    SDL_RenderCopy(sdl.renderer, sdl.texture, nullptr, nullptr);
    SDL_RenderPresent(sdl.renderer);
}

// Sleeps until `deadline` (performance-counter ticks). Coarse SDL_Delay first,
// then a short spin for sub-millisecond precision.
void wait_until(u64 deadline, u64 freq) {
    for (;;) {
        const u64 now = SDL_GetPerformanceCounter();
        if (now >= deadline) return;
        const u64 remaining_ms = (deadline - now) * 1000 / freq;
        if (remaining_ms > 2) SDL_Delay(static_cast<u32>(remaining_ms - 1));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    SdlContext sdl;

    // Hardware components are large; keep them off the stack.
    auto system = std::make_unique<core::System>();
    frontend::SdlInput input;

    const char* bios_path = nullptr;
    const char* disc_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--mmio-report")
            system->set_mmio_report_interval(120);  // every ~2 s
        else if (arg == "--pal")
            system->set_region(core::Region::Pal);
        else if (arg == "--ntsc")
            system->set_region(core::Region::Ntsc);
        else if (!bios_path)
            bios_path = argv[i];
        else
            disc_path = argv[i];
    }
    if (disc_path && !system->insert_disc(disc_path)) return 1;

    if (bios_path) {
        const auto bios = read_file(bios_path);
        if (!bios) {
            Log::error("Cannot read BIOS file '{}'", bios_path);
            return 1;
        }
        if (!system->load_bios(*bios)) return 1;
        Log::info("Loaded BIOS '{}' ({} bytes)", bios_path, bios->size());
    } else {
        Log::info("No BIOS given (usage: 3do-emu [--pal|--ntsc] [--mmio-report] <bios.bin> [disc.iso|.bin|.chd]); "
                  "CPU idle, test pattern only");
    }

    // The window matches the machine's output: the demo, or NTSC/PAL video.
    if (!init_sdl(sdl, system->screen_width(), system->screen_height())) return 1;
    update_title(sdl, *system);

    const u64 freq = SDL_GetPerformanceFrequency();
    const auto ticks_per_frame = static_cast<u64>(static_cast<double>(freq) / system->field_rate_hz());
    u64 next_frame = SDL_GetPerformanceCounter() + ticks_per_frame;

    Log::info("Entering main loop at {:.2f} Hz", system->field_rate_hz());

    bool running = true;
    while (running) {
        running = handle_events(input, *system, sdl);  // 1. Input / window events
        input.update(system->player_bus()); //    latch pads for this frame's PBUS DMA
        system->run_frame();                // 2. Emulate one frame of hardware
        render(sdl, *system);               // 3. Present the framebuffer
        play_audio(sdl, *system);           //    and queue its audio
        wait_until(next_frame, freq);       // 4. Pace to 60 Hz

        next_frame += ticks_per_frame;
        // If we fell far behind (debugger pause, window drag), resync instead of
        // fast-forwarding to catch up.
        const u64 now = SDL_GetPerformanceCounter();
        if (now > next_frame + ticks_per_frame * 5) next_frame = now + ticks_per_frame;
    }

    Log::info("Exiting after {} frames", system->frame_count());
    return 0;
}
