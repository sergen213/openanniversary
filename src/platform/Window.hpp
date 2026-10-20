#pragma once
// Native window and input front end (SDL3). Deliberately small: the renderer needs a surface provider, the game
// loop needs close/quit handling and key state; nothing else lives here.

#include <string>

struct SDL_Window;

namespace oa::platform {

struct WindowConfig {
    std::string title = "OpenAnniversary";
    int width = 1600;
    int height = 900;
    bool resizable = true;
};

class Window {
public:
    // One-time SDL initialisation for the video subsystem. Returns false with a message when no display is
    // reachable (for example a headless shell without Xvfb), which callers use to pick a self-check path.
    static bool initVideo(std::string* error = nullptr);
    static void quitVideo();

    ~Window();

    bool create(const WindowConfig& config, std::string* error = nullptr);
    void destroy();

    int widthPixels() const;   // drawable size, not the logical size under fractional scaling
    int heightPixels() const;
    SDL_Window* sdl() const { return window_; }

    // Pump once. Sets closed_ on a quit request or window close; Escape also closes so a stuck window is escapable.
    void pumpEvents();
    bool shouldClose() const { return closed_; }
    bool keyDown(int sdlScancode) const;

private:
    SDL_Window* window_ = nullptr;
    bool closed_ = false;
};

}  // namespace oa::platform
