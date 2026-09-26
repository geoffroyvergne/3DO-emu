#include "frontend/sdl_input.hpp"

#include <algorithm>
#include <utility>

#include "common/log.hpp"

namespace frontend {

namespace {

using core::input::PadButton;
using core::input::PadState;

constexpr Sint16 kStickDeadzone = 8000;

constexpr std::pair<SDL_Scancode, PadButton> kKeyMap[] = {
    {SDL_SCANCODE_UP, PadButton::Up},       {SDL_SCANCODE_DOWN, PadButton::Down},
    {SDL_SCANCODE_LEFT, PadButton::Left},   {SDL_SCANCODE_RIGHT, PadButton::Right},
    {SDL_SCANCODE_Z, PadButton::A},         {SDL_SCANCODE_X, PadButton::B},
    {SDL_SCANCODE_C, PadButton::C},         {SDL_SCANCODE_A, PadButton::L},
    {SDL_SCANCODE_S, PadButton::R},         {SDL_SCANCODE_RETURN, PadButton::P},
    {SDL_SCANCODE_BACKSPACE, PadButton::X},
};

constexpr std::pair<SDL_GameControllerButton, PadButton> kButtonMap[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP, PadButton::Up},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PadButton::Down},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PadButton::Left},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PadButton::Right},
    {SDL_CONTROLLER_BUTTON_X, PadButton::A},
    {SDL_CONTROLLER_BUTTON_A, PadButton::B},
    {SDL_CONTROLLER_BUTTON_B, PadButton::C},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PadButton::L},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PadButton::R},
    {SDL_CONTROLLER_BUTTON_START, PadButton::P},
    {SDL_CONTROLLER_BUTTON_BACK, PadButton::X},
};

PadState merge(PadState a, PadState b) {
    return {static_cast<u16>(a.buttons | b.buttons)};
}

}  // namespace

SdlInput::~SdlInput() {
    for (SDL_GameController* controller : controllers_) SDL_GameControllerClose(controller);
}

void SdlInput::handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            // `which` is a device index here.
            SDL_GameController* controller = SDL_GameControllerOpen(event.cdevice.which);
            if (!controller) {
                Log::warn("Cannot open controller {}: {}", event.cdevice.which, SDL_GetError());
                break;
            }
            if (std::ranges::find(controllers_, controller) != controllers_.end()) {
                SDL_GameControllerClose(controller);  // already open: drop the extra reference
                break;
            }
            controllers_.push_back(controller);
            Log::info("Controller connected: {} (3DO pad {})", SDL_GameControllerName(controller),
                      controllers_.size());
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            // `which` is an instance id here.
            const auto it = std::ranges::find_if(controllers_, [&](SDL_GameController* c) {
                return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == event.cdevice.which;
            });
            if (it == controllers_.end()) break;
            Log::info("Controller disconnected: {}", SDL_GameControllerName(*it));
            SDL_GameControllerClose(*it);
            controllers_.erase(it);
            break;
        }
        default: break;
    }
}

void SdlInput::update(core::input::PlayerBus& bus) const {
    const std::size_t pads = std::clamp<std::size_t>(controllers_.size(), 1,
                                                     core::input::PlayerBus::kMaxPads);
    bus.set_pad_count(pads);

    PadState first = read_keyboard();
    if (!controllers_.empty()) first = merge(first, read_controller(controllers_[0]));
    bus.set_pad(0, first);
    for (std::size_t i = 1; i < pads; ++i) bus.set_pad(i, read_controller(controllers_[i]));
}

PadState SdlInput::read_keyboard() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    PadState pad;
    for (const auto& [scancode, button] : kKeyMap) pad.set(button, keys[scancode] != 0);
    return pad;
}

PadState SdlInput::read_controller(SDL_GameController* controller) {
    PadState pad;
    for (const auto& [sdl_button, button] : kButtonMap)
        if (SDL_GameControllerGetButton(controller, sdl_button)) pad.set(button, true);

    const Sint16 x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    if (x < -kStickDeadzone) pad.set(PadButton::Left, true);
    if (x > kStickDeadzone) pad.set(PadButton::Right, true);
    if (y < -kStickDeadzone) pad.set(PadButton::Up, true);
    if (y > kStickDeadzone) pad.set(PadButton::Down, true);
    return pad;
}

}  // namespace frontend
