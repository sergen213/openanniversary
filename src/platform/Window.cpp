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
    OA_LOG(LogLevel::Info, "platform", "window {}x{} drawable-ready '{}'", config.width, config.height, config.title);
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
    return states != nullptr && index >= 0 && index < static_cast<int>(std::size(kScancode)) && kScancode[index] < count &&
           states[kScancode[index]];
}

}  // namespace oa::platform
