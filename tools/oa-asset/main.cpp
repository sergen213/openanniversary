// oa-asset: inspect and extract the cdc bigfile archive of a Legend-era install (read-only).
//
//   oa-asset <game-dir> list [--mask HEX]                 dump the record table
//   oa-asset <game-dir> hash <path>...                    hashes the game would search for
//   oa-asset <game-dir> resolve <paths.txt>               pair candidate paths with the record table
//   oa-asset <game-dir> extract --hash HEX [--mask HEX] --out FILE
//   oa-asset <game-dir> extract --path "pc-w\lara.drm" --out FILE
//
// Records are stored unlabelled; names come from outside (see docs/formats/CONTAINERS.md).

#include "cdc/Bigfile.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path directory;
    std::string command;
    std::string path;
    std::string pathsFile;
    std::string out;
    bool haveHash = false;
    std::uint32_t hash = 0;
    std::uint32_t mask = oa::cdc::kMaskAll;
    std::vector<std::string> positional;
};

bool parseHex(const std::string& text, std::uint32_t& value) {
    std::istringstream in(text);
    unsigned long long parsed = 0;
    if (!(in >> std::hex >> parsed) || !in.eof() || parsed > 0xFFFFFFFFull) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parseArgs(int argc, char** argv, Options& opt, std::string& error) {
    if (argc < 3) {
        error = "usage: oa-asset <game-dir> <list|hash|resolve|extract> [options]";
        return false;
    }
    opt.directory = argv[1];
    opt.command = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](std::string& store) -> bool {
            if (i + 1 >= argc) {
                error = "missing value after " + arg;
                return false;
            }
            store = argv[++i];
            return true;
        };
        if (arg == "--out") {
            std::string text;
            if (!value(text)) return false;
            opt.out = text;
        } else if (arg == "--path") {
            if (!value(opt.path)) return false;
        } else if (arg == "--paths") {
            if (!value(opt.pathsFile)) return false;
        } else if (arg == "--mask" || arg == "--hash") {
            std::string text;
            if (!value(text) || !parseHex(text, arg == "--mask" ? opt.mask : opt.hash)) {
                if (error.empty()) error = "bad " + arg + ": " + text;
                return false;
            }
            if (arg == "--hash") opt.haveHash = true;
        } else if (!arg.empty() && arg[0] == '-') {
            error = "unknown option " + arg;
            return false;
        } else {
            opt.positional.push_back(arg);
        }
    }
    return true;
}

int fail(const std::string& message) {
    std::cerr << "oa-asset: " << message << '\n';
    return 1;
}

void printRecord(const oa::cdc::Record& record, const char* name = nullptr) {
    const oa::cdc::PartRef ref = oa::cdc::splitAtSector(record.sector, oa::cdc::kLegendPartAlignment);
    std::printf("%08X size=%-9u part=%llu offset=%-10llu specMask=%08X comp=%u", record.hash, record.size,
                static_cast<unsigned long long>(ref.part), static_cast<unsigned long long>(ref.offset),
                record.specMask, record.compressedSize);
    if (name != nullptr) std::printf(" path=%s", name);
    std::putchar('\n');
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string error;
    if (!parseArgs(argc, argv, opt, error)) return fail(error);

    oa::cdc::BigfileSet archive;
    if (!archive.open(opt.directory, &error)) return fail(error);
    archive.setSpecMask(opt.mask);

    if (opt.command == "list") {
        for (const auto& record : archive.records()) printRecord(record);
        std::fprintf(stderr, "%zu records in %zu parts\n", archive.records().size(), archive.partCount());
        return 0;
    }

    if (opt.command == "hash") {
        for (const std::string& path : opt.positional) {
            std::printf("%08X %s\n", oa::cdc::pathHash(path), path.c_str());
        }
        return 0;
    }

    if (opt.command == "resolve") {
        if (opt.pathsFile.empty() && !opt.positional.empty()) opt.pathsFile = opt.positional.front();
        std::ifstream in(opt.pathsFile);
        if (!in) return fail("cannot read " + opt.pathsFile);
        std::string line;
        std::size_t matched = 0, total = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            ++total;
            if (const auto index = archive.find(line, opt.mask)) {
                printRecord(archive.records()[*index], line.c_str());
                ++matched;
            }
        }
        std::fprintf(stderr, "matched %zu of %zu candidate paths\n", matched, total);
        return matched != 0 ? 0 : 1;
    }

    if (opt.command == "extract") {
        std::optional<std::size_t> index;
        if (opt.haveHash) {
            for (std::size_t i = 0; i < archive.records().size(); ++i) {
                const auto& record = archive.records()[i];
                if (record.hash == opt.hash && (record.specMask & opt.mask) == opt.mask) {
                    index = i;
                    break;
                }
            }
        } else if (!opt.path.empty()) {
            index = archive.find(opt.path, opt.mask);
        } else {
            return fail("extract needs --hash or --path");
        }
        if (!index) return fail("no record for the requested hash/path at mask " + std::to_string(opt.mask));

        const auto data = archive.read(*index, &error);
        if (data.empty() && !error.empty()) return fail(error);
        if (opt.out.empty()) {
            std::fwrite(data.data(), 1, data.size(), stdout);
            return 0;
        }
        std::FILE* out = std::fopen(opt.out.c_str(), "wb");
        if (out == nullptr) return fail("cannot write " + opt.out);
        const std::size_t written = std::fwrite(data.data(), 1, data.size(), out);
        std::fclose(out);
        if (written != data.size()) return fail("short write to " + opt.out);
        std::fprintf(stderr, "wrote %zu bytes to %s\n", data.size(), opt.out.c_str());
        return 0;
    }

    return fail("unknown command '" + opt.command + "'");
}
