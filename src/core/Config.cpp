#include "core/Config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace oa {
namespace {

std::string normalize(std::string_view key) {
    std::string out;
    for (char c : key) out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    return out;
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

}  // namespace

void Config::setDefault(std::string key, std::string value) {
    key = normalize(key);
    const auto at = std::ranges::lower_bound(values_, key, {}, &std::pair<std::string, std::string>::first);
    if (at != values_.end() && at->first == key) return;  // an explicit layer already owns this key
    values_.insert(at, {std::move(key), std::move(value)});
}

bool Config::loadFile(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error != nullptr) *error = "cannot read " + path.string();
        return false;
    }
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.erase(comment);
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (!trim(line).empty() && error != nullptr) {
                *error = path.string() + ":" + std::to_string(lineNumber) + ": expected key = value";
                return false;
            }
            continue;
        }
        const std::string key = normalize(trim(std::string_view(line).substr(0, eq)));
        if (key.empty()) {
            if (error != nullptr) *error = path.string() + ":" + std::to_string(lineNumber) + ": empty key";
            return false;
        }
        const std::string value = trim(std::string_view(line).substr(eq + 1));
        const auto at = std::ranges::lower_bound(values_, key, {}, &std::pair<std::string, std::string>::first);
        if (at != values_.end() && at->first == key) {
            at->second = value;  // last definition wins
        } else {
            values_.insert(at, {key, value});
        }
    }
    return true;
}

bool Config::applyCommandLine(int argc, char** argv, std::vector<std::string>* rest, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0 || arg.size() < 3) {
            if (rest != nullptr) rest->push_back(arg);
            continue;
        }
        const std::size_t eq = arg.find('=');
        const std::string key = normalize(std::string_view(arg).substr(2, eq == std::string::npos ? std::string::npos : eq - 2));
        if (key.empty()) {
            if (error != nullptr) *error = "malformed option '" + arg + "'";
            return false;
        }
        const std::string value = (eq == std::string::npos) ? "true" : trim(std::string_view(arg).substr(eq + 1));
        const auto at = std::ranges::lower_bound(values_, key, {}, &std::pair<std::string, std::string>::first);
        if (at != values_.end() && at->first == key) {
            at->second = value;  // command line overrides files and defaults
        } else {
            values_.insert(at, {key, value});
        }
    }
    return true;
}

std::optional<std::string> Config::find(std::string_view key) const {
    const std::string wanted = normalize(key);
    const auto at = std::ranges::lower_bound(values_, wanted, {}, &std::pair<std::string, std::string>::first);
    if (at == values_.end() || at->first != wanted) return std::nullopt;
    return at->second;
}

std::string Config::str(std::string_view key, std::string fallback) const {
    const auto value = find(key);
    return value.value_or(fallback);
}

int Config::integer(std::string_view key, int fallback) const {
    const auto value = find(key);
    if (!value.has_value()) return fallback;
    std::istringstream in(*value);
    int parsed = 0;
    if (!(in >> parsed) || !in.eof()) return fallback;
    return parsed;
}

bool Config::flag(std::string_view key, bool fallback) const {
    const auto value = find(key);
    if (!value.has_value()) return fallback;
    std::string text = normalize(*value);
    if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
    if (text == "0" || text == "false" || text == "no" || text == "off") return false;
    return fallback;
}

std::vector<std::pair<std::string, std::string>> Config::entries() const { return values_; }

}  // namespace oa
