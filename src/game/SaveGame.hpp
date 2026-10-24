#pragma once
// Save/load for the driven body (docs/ROADMAP.md Milestone 6): one text slot per level, written by hand - no engine
// save format is evidenced in the shipped game data we have read, so this file describes OUR state only.
// The contract is a lossy float round-trip at std::to_string precision; positions under ~1e7 units keep centimetre
// fidelity at GEOMETRY.md §13's scale.

#include <filesystem>
#include <string>
#include <string_view>

#include "game/Avatar.hpp"

namespace oa::game {

struct SaveState {
    std::string level;   // the record label the app loaded, e.g. "8baf540e" or "pc-w\\ma1.drm"
    AvatarState avatar;
    float yaw = 0.0f;    // camera framing at save time: restoring these re-frames the body without a follow-latch frame
    float pitch = 0.0f;
    float zoom = 1.0f;
};

std::string toText(const SaveState& state);

// True when `text` parses into *out. Every numeric key must be present and finite; the level name must be non-empty;
// a non-positive zoom is rejected (it would invert or collapse the projection). Unknown keys are ignored, so an older
// file with fewer fields still loads.
bool fromText(std::string_view text, SaveState* out, std::string* error = nullptr);

bool saveToFile(const std::filesystem::path& path, const SaveState& state, std::string* error = nullptr);
bool loadFromFile(const std::filesystem::path& path, SaveState* out, std::string* error = nullptr);

}  // namespace oa::game
