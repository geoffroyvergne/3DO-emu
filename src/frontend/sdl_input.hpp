#pragma once

#include <SDL.h>

#include <vector>

#include "core/input/pbus.hpp"

namespace frontend {

// Maps SDL keyboard and game controllers onto 3DO control pads.
// Pad 0 merges the keyboard with the first controller; each further
// controller becomes the next pad in the daisy chain.
//
// Keyboard:   arrows = D-pad, Z/X/C = A/B/C, A/S = L/R, Enter = P, Backspace = X
// Controller: D-pad or left stick; X/A/B (the left, bottom and right face
//             buttons, SDL's Xbox naming) = 3DO A/B/C; shoulders = L/R;
//             Start = P, Back = X
class SdlInput {
public:
    SdlInput() = default;
    ~SdlInput();
    SdlInput(const SdlInput&) = delete;
    SdlInput& operator=(const SdlInput&) = delete;

    // Controller hot-plug. SDL also reports controllers present at startup this way.
    void handle_event(const SDL_Event& event);

    // Copies the current input state into the PBUS chain.
    void update(core::input::PlayerBus& bus) const;

private:
    [[nodiscard]] static core::input::PadState read_keyboard();
    [[nodiscard]] static core::input::PadState read_controller(SDL_GameController* controller);

    std::vector<SDL_GameController*> controllers_;
};

}  // namespace frontend
