#include "platform/Audio.hpp"

#include "core/Log.hpp"

#include <SDL3/SDL.h>
#include <string>

namespace oa::platform {

Audio::~Audio() { finish(); }

bool Audio::create(int rateHz, std::string* error) {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        if (error != nullptr) *error = "SDL audio init failed: " + std::string(SDL_GetError());
        return false;
    }
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (devices == nullptr || count == 0) {
        if (error != nullptr) *error = "the platform reports no audio playback device";
        return false;
    }
    const SDL_AudioDeviceID physical = devices[0];
    SDL_free(devices);
    device_ = SDL_OpenAudioDevice(physical, nullptr);
    if (device_ == 0u) {
        if (error != nullptr) *error = "SDL_OpenAudioDevice failed: " + std::string(SDL_GetError());
        return false;
    }
    // The device's own name and format are logged rather than assumed, so a run on another
    // machine can be compared against its numbers.
    SDL_AudioSpec deviceFormat{};
    SDL_GetAudioDeviceFormat(device_, &deviceFormat, nullptr);
    SDL_AudioSpec mine{};
    mine.format = SDL_AUDIO_S16;
    mine.channels = 1;
    mine.freq = rateHz;
    stream_ = SDL_CreateAudioStream(&mine, &mine);
    if (stream_ == nullptr || !SDL_BindAudioStream(device_, stream_)) {
        if (error != nullptr) *error = "could not bind an audio stream to the device";
        finish();
        return false;
    }
    const char* name = SDL_GetAudioDeviceName(device_);
    OA_LOG(oa::LogLevel::Info, "platform",
           "audio device '{}' at {} Hz x {} channel(s) plays our mono S16 @ {} Hz stream",
           (name != nullptr ? name : "(unnamed)"), deviceFormat.freq, deviceFormat.channels, rateHz);
    return true;
}

void Audio::push(const std::int16_t* samples, std::size_t count) {
    if (stream_ == nullptr) return;
    SDL_PutAudioStreamData(stream_, samples, static_cast<int>(count * 2u));
}

void Audio::finish() {
    if (stream_ != nullptr) {
        SDL_FlushAudioStream(stream_);
        SDL_UnbindAudioStream(stream_);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (device_ != 0u) {
        SDL_CloseAudioDevice(device_);
        device_ = 0u;
    }
}

}  // namespace platform
