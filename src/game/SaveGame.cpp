#include "game/SaveGame.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace oa::game {
namespace {

void putFloat(std::ostringstream& out, const char* key, float value) {
    out << key << '=' << std::to_string(value) << '\n';
}

bool parseFloat(std::string_view text, float* out) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, *out);
    return result.ec == std::errc{} && result.ptr == end;
}

bool getFloat(const std::unordered_map<std::string, std::string>& fields, const char* key, float* out) {
    const auto it = fields.find(key);
    return it != fields.end() && parseFloat(it->second, out) && std::isfinite(*out);
}

}  // namespace

std::string toText(const SaveState& state) {
    std::ostringstream out;
    out << "level=" << state.level << '\n';
    putFloat(out, "x", state.avatar.x);
    putFloat(out, "y", state.avatar.y);
    putFloat(out, "z", state.avatar.z);
    putFloat(out, "vx", state.avatar.vx);
    putFloat(out, "vy", state.avatar.vy);
    putFloat(out, "vz", state.avatar.vz);
    out << "grounded=" << (state.avatar.grounded ? 1 : 0) << '\n';
    putFloat(out, "groundY", state.avatar.groundY);
    out << "groundTriangle=" << state.avatar.groundTriangle << '\n';
    putFloat(out, "yaw", state.yaw);
    putFloat(out, "pitch", state.pitch);
    putFloat(out, "zoom", state.zoom);
    return out.str();
}

bool fromText(std::string_view text, SaveState* out, std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) *error = "fromText needs an output object";
        return false;
    }
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    std::unordered_map<std::string, std::string> fields;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;   // tolerate comments/blank lines
        fields[line.substr(0, eq)] = line.substr(eq + 1);
    }
    SaveState state;
    const auto level = fields.find("level");
    if (level == fields.end() || level->second.empty()) return fail("save file names no level");
    state.level = level->second;
    float groundedValue = 0.0f;
    const bool ok = getFloat(fields, "x", &state.avatar.x) && getFloat(fields, "y", &state.avatar.y) &&
                    getFloat(fields, "z", &state.avatar.z) && getFloat(fields, "vx", &state.avatar.vx) &&
                    getFloat(fields, "vy", &state.avatar.vy) && getFloat(fields, "vz", &state.avatar.vz) &&
                    getFloat(fields, "groundY", &state.avatar.groundY) &&
                    getFloat(fields, "yaw", &state.yaw) && getFloat(fields, "pitch", &state.pitch) &&
                    getFloat(fields, "zoom", &state.zoom) && getFloat(fields, "grounded", &groundedValue);
    if (!ok) return fail("save file is missing a numeric field");
    state.avatar.grounded = groundedValue != 0.0f;
    // groundTriangle names the contact triangle inside TerrainQuery's private grid; an older slot without it just
    // re-probes on the first tick, so the key stays optional by contract (the header states this).
    float triangleValue = 0.0f;
    getFloat(fields, "groundTriangle", &triangleValue);
    state.avatar.groundTriangle = static_cast<std::uint32_t>(triangleValue);
    if (state.zoom <= 0.0f) return fail("save file zoom is not positive");
    *out = state;
    return true;
}

bool saveToFile(const std::filesystem::path& path, const SaveState& state, std::string* error) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error != nullptr) *error = "cannot open " + path.string() + " for writing";
        return false;
    }
    file << toText(state);
    if (!file) {
        if (error != nullptr) *error = "writing " + path.string() + " failed";
        return false;
    }
    return true;
}

bool loadFromFile(const std::filesystem::path& path, SaveState* out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) *error = "no save file at " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return fromText(buffer.str(), out, error);
}

}  // namespace oa::game
