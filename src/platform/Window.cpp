#include "platform/Window.hpp"

#include "core/Log.hpp"
#include <SDL3/SDL.h>

namespace oa::platform {

bool Window::initVideo(std::string* error) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (error != nullptr) *error = std::string("SDL video init failed: ") + SDL_GetError();
        return false;
    }
    return true;
}

void Window::quitVideo() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }

Window::~Window() { destroy(); }

bool Window::create(const WindowConfig& config, std::string* error) {
    Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
    window_ = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (window_ == nullptr) {
        if (error != nullptr) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }
    int logicalW = 0, logicalH = 0, deviceW = 0, deviceH = 0;
    SDL_GetWindowSize(window_, &logicalW, &logicalH);
    SDL_GetWindowSizeInPixels(window_, &deviceW, &deviceH);
    // The two sizes differ exactly when the compositor scales the window (HiDPI): the renderer works in
    // device pixels throughout, so a scaled display renders sharper rather than resampled.
    OA_LOG(LogLevel::Info, "platform", "window '{}': logical {}x{}, device {}x{}, pixel density {}",
           config.title, logicalW, logicalH, deviceW, deviceH, SDL_GetWindowPixelDensity(window_));
    return true;
}

void Window::destroy() {
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    closed_ = false;
}

int Window::widthPixels() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return w;
}

int Window::heightPixels() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return h;
}

void Window::pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                closed_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) closed_ = true;
                break;
            default:
                break;
        }
    }
}

bool Window::keyDown(Key key) const {
    // Index order must match enum class Key in Window.hpp; a mismatch would silently swap the controls.
    static constexpr int kScancode[] = {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
                                        SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D,
                                        SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT};
    const int index = static_cast<int>(key);
    int count = 0;
    const bool* states = SDL_GetKeyboardState(&count);
    const bool keyboard = states != nullptr && index >= 0 && index < static_cast<int>(std::size(kScancode)) &&
                          kScancode[index] < count && states[kScancode[index]];
    return keyboard || gamepadDown(key);
}

bool Window::enableGamepad(std::string* error) {
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    if (pads == nullptr || count == 0) {
        if (error != nullptr) *error = "the platform reports no gamepad";
        return false;
    }
    gamepad_ = static_cast<int>(pads[0]);
    SDL_free(pads);
    SDL_Gamepad* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(gamepad_));
    OA_LOG(LogLevel::Info, "platform", "gamepad '{}' vendor {} adopted: left stick and d-pad are the named keys (and the WASD aliases), "
                                       "South jumps, East runs, shoulder buttons dolly",
           (pad != nullptr ? SDL_GetGamepadName(pad) : "(detached)"),
           (pad != nullptr ? SDL_GetGamepadVendor(pad) : 0));
    return true;
}

bool Window::gamepadDown(Key key) const {
    if (gamepad_ == 0) return false;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(gamepad_));
    if (pad == nullptr) return false;
    // Half-scale dead zone on the stick; the d-pad buttons answer the same keys.
    const auto stickLeft = [&]() {
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) < -8000 ||
               SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    };
    const auto stickRight = [&]() {
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) > 8000 ||
               SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    };
    const auto stickUp = [&]() {   // the stick's negative Y is up, like the screen
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) < -8000 ||
               SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    };
    const auto stickDown = [&]() {
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) > 8000 ||
               SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    };
    switch (key) {
        case Key::Left:  return stickLeft();
        case Key::Right: return stickRight();
        case Key::Up:    return stickUp();
        case Key::Down:  return stickDown();
        case Key::W:     return stickUp();
        case Key::A:     return stickLeft();
        case Key::S:     return stickDown();
        case Key::D:     return stickRight();
        case Key::Q:     return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        case Key::E:     return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        case Key::Jump:  return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
        case Key::Run:   return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
        default:         return false;
    }
}

}  // namespace oa::platform
