#pragma once
// Key/value configuration with defaults, a file layer and a command-line layer (highest precedence).
// Keys are ASCII case-insensitive; values keep their text form until a typed getter asks for one.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa {

class Config {
public:
    // A default only applies while no higher layer has set the key; defaults never overwrite values.
    void setDefault(std::string key, std::string value);

    // "key = value" lines, '#' or ';' start a comment, later definitions of the same key win.
    bool loadFile(const std::filesystem::path& path, std::string* error = nullptr);

    // "--key=value" sets a value; a bare "--key" stores "true". argv[0] is consumed and every other
    // non-option argument is returned through `rest`.
    bool applyCommandLine(int argc, char** argv, std::vector<std::string>* rest = nullptr,
                          std::string* error = nullptr);

    std::optional<std::string> find(std::string_view key) const;
    std::string str(std::string_view key, std::string fallback = {}) const;
    int integer(std::string_view key, int fallback) const;
    bool flag(std::string_view key, bool fallback) const;  // 1/0 true/false yes/no on/off
    std::vector<std::pair<std::string, std::string>> entries() const;  // sorted by key

private:
    std::vector<std::pair<std::string, std::string>> values_;  // kept sorted for stable dumps and lookups
};

}  // namespace oa
