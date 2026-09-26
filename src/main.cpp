#include <SDL.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/log.hpp"
#include "common/types.hpp"
#include "core/system.hpp"
#include "frontend/sdl_input.hpp"

namespace {

constexpr int kWindowScale = 3;

struct SdlContext {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;

    ~SdlContext() {
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

bool init_sdl(SdlContext& sdl, int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
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
    return true;
}

// Returns false when the user asked to quit.
bool handle_events(frontend::SdlInput& input) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        input.handle_event(event);
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) return false;
                break;
            default:
                break;
        }
    }
    return true;
}

void render(SdlContext& sdl, const core::System& system) {
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

std::optional<std::vector<u8>> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char* argv[]) {
    SdlContext sdl;

    // Hardware components are large; keep them off the stack.
    auto system = std::make_unique<core::System>();
    frontend::SdlInput input;

    const char* bios_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--mmio-report")
            system->set_mmio_report_interval(120);  // every ~2 s
        else if (arg == "--pal")
            system->set_region(core::Region::Pal);
        else
            bios_path = argv[i];
    }

    if (bios_path) {
        const auto bios = read_file(bios_path);
        if (!bios) {
            Log::error("Cannot read BIOS file '{}'", bios_path);
            return 1;
        }
        if (!system->load_bios(*bios)) return 1;
        Log::info("Loaded BIOS '{}' ({} bytes)", bios_path, bios->size());
    } else {
        Log::info("No BIOS given (usage: 3do-emu [--pal] [--mmio-report] <bios.bin>); "
                  "CPU idle, test pattern only");
    }

    // The window matches the machine's output: the demo, or NTSC/PAL video.
    if (!init_sdl(sdl, system->screen_width(), system->screen_height())) return 1;

    const u64 freq = SDL_GetPerformanceFrequency();
    const auto ticks_per_frame = static_cast<u64>(static_cast<double>(freq) / system->field_rate_hz());
    u64 next_frame = SDL_GetPerformanceCounter() + ticks_per_frame;

    Log::info("Entering main loop at {:.2f} Hz", system->field_rate_hz());

    bool running = true;
    while (running) {
        running = handle_events(input);     // 1. Input / window events
        input.update(system->player_bus()); //    latch pads for this frame's PBUS DMA
        system->run_frame();                // 2. Emulate one frame of hardware
        render(sdl, *system);               // 3. Present the framebuffer
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
