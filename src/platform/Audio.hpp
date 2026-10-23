#pragma once
// The audio counterpart of Window: the platform's first playback device, handed a stream of decoded
// samples. Deliberately small — the game hands PCM to the device, the device plays it.

#include <cstddef>
#include <cstdint>
#include <string>

struct SDL_AudioStream;

namespace oa::platform {

class Audio {
public:
    ~Audio();   // closes what it opened, so early returns are safe

    // Open the platform's first playback device and bind a mono 16-bit stream at the given rate.
    // False with a message when the platform reports no device (a headless shell included).
    bool create(int rateHz, std::string* error = nullptr);

    // Hand samples to the bound stream; the device consumes them on its callback thread.
    void push(const std::int16_t* samples, std::size_t count);

    // Flush and close everything. ~Audio() calls this; call it early when you want the samples
    // guaranteed delivered before the run reports.
    void finish();

private:
    unsigned int device_ = 0u;   // SDL_AudioDeviceID is an unsigned int
    SDL_AudioStream* stream_ = nullptr;
};

}  // namespace platform
