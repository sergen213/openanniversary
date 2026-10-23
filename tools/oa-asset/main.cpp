// oa-asset: inspect and extract the cdc bigfile archive of a Legend-era install (read-only).
//
//   oa-asset <game-dir> list [--mask HEX]                 dump the record table
//   oa-asset <game-dir> hash <path>...                    hashes the game would search for
//   oa-asset <game-dir> resolve <paths.txt>               pair candidate paths with the record table
//   oa-asset <game-dir> extract --hash HEX [--mask HEX] --out FILE
//   oa-asset <game-dir> extract --path "pc-w\lara.drm" --out FILE
//   oa-asset <game-dir> drm --path PATH | --hash HEX [--section N --out FILE]
//   oa-asset <game-dir> namescan [--out FILE]             pair type-7 event keys with sound-stream records
//   oa-asset <game-dir> texture --path PATH | --hash HEX [--section N] --out FILE.png
//
// Numbers are read as hexadecimal everywhere, including --section (--section 12 is unit section 18).
//
// Records are stored unlabelled; names come from outside (see docs/formats/CONTAINERS.md).

#include "cdc/Bigfile.hpp"
#include "cdc/Drm.hpp"
#include "cdc/Geometry.hpp"
#include "cdc/Texture.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <span>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <set>
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
    bool haveSection = false;
    std::uint32_t section = 0;
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
        error = "usage: oa-asset <game-dir> <list|hash|resolve|extract|drm|namescan|texture|terrain|terrper> [options]";
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
        } else if (arg == "--section") {
            std::string text;
            if (!value(text) || !parseHex(text, opt.section)) return false;
            opt.haveSection = true;
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

// Resolves --hash or --path to a record index under the current spec mask.
std::optional<std::size_t> findRecord(const oa::cdc::BigfileSet& archive, const Options& opt) {
    if (opt.haveHash) {
        for (std::size_t i = 0; i < archive.records().size(); ++i) {
            const auto& record = archive.records()[i];
            if (record.hash == opt.hash && (record.specMask & opt.mask) == opt.mask) return i;
        }
        return std::nullopt;
    }
    if (!opt.path.empty()) return archive.find(opt.path, opt.mask);
    return std::nullopt;
}

void printRecord(const oa::cdc::Record& record, const char* name = nullptr) {
    const oa::cdc::PartRef ref = oa::cdc::splitAtSector(record.sector, oa::cdc::kLegendPartAlignment);
    std::printf("%08X size=%-9u part=%llu offset=%-10llu specMask=%08X comp=%u", record.hash, record.size,
                static_cast<unsigned long long>(ref.part), static_cast<unsigned long long>(ref.offset),
                record.specMask, record.compressedSize);
    if (name != nullptr) std::printf(" path=%s", name);
    std::putchar('\n');
}

std::array<unsigned char, 3> unpack565(std::uint16_t value) {
    const unsigned r = (value >> 11u) & 0x1Fu;
    const unsigned g = (value >> 5u) & 0x3Fu;
    const unsigned b = value & 0x1Fu;
    return {static_cast<unsigned char>((r << 3u) | (r >> 2u)), static_cast<unsigned char>((g << 2u) | (g >> 4u)),
            static_cast<unsigned char>((b << 3u) | (b >> 2u))};
}

std::uint16_t readU16At(const std::byte* at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(*at) |
                                      (static_cast<unsigned char>(*(at + 1)) << 8));
}

// BC1: two reference colours plus two-bit selectors. c0 > c1 gives four opaque interpolated colours,
// otherwise selector 3 is transparent and is drawn black here because PPM/PNG has no alpha.
void decodeBc1(const std::byte* block, unsigned char* rgb) {
    const auto c0 = unpack565(readU16At(block));
    const auto c1 = unpack565(readU16At(block + 2));
    unsigned char palette[4][3];
    for (int i = 0; i < 3; ++i) {
        palette[0][i] = c0[i];
        palette[1][i] = c1[i];
    }
    if (readU16At(block) > readU16At(block + 2)) {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<unsigned char>((2u * c0[i] + c1[i]) / 3u);
            palette[3][i] = static_cast<unsigned char>((c0[i] + 2u * c1[i]) / 3u);
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<unsigned char>((c0[i] + c1[i]) / 2u);
            palette[3][i] = 0;
        }
    }
    for (int row = 0; row < 4; ++row) {
        const unsigned bits = static_cast<unsigned char>(block[4 + row]);
        for (int col = 0; col < 4; ++col) {
            const int index = (bits >> (2 * col)) & 3u;
            unsigned char* out = rgb + 3 * (row * 4 + col);
            out[0] = palette[index][0];
            out[1] = palette[index][1];
            out[2] = palette[index][2];
        }
    }
}

// BC3: an interpolated 8-entry alpha table with three-bit selectors, then a BC1 block. Alpha is folded
// into the RGB so the result is visible against the PNG's white background check.
void decodeBc3(const std::byte* block, unsigned char* rgb) {
    unsigned char alphas[8];
    const int a0 = static_cast<unsigned char>(block[0]);
    const int a1 = static_cast<unsigned char>(block[1]);
    if (a0 > a1) {
        alphas[0] = static_cast<unsigned char>(a0);
        alphas[1] = static_cast<unsigned char>(a1);
        for (int i = 1; i < 6; ++i) {
            alphas[1 + i] = static_cast<unsigned char>(((6 - i) * a0 + i * a1) / 6);
        }
    } else {
        alphas[0] = static_cast<unsigned char>(a0);
        alphas[1] = static_cast<unsigned char>(a1);
        for (int i = 1; i < 4; ++i) {
            alphas[1 + i] = static_cast<unsigned char>(((4 - i) * a0 + i * a1) / 4);
        }
        alphas[5] = 0;
        alphas[6] = 255;
        alphas[7] = 0;
    }
    decodeBc1(block + 8, rgb);
    for (int texel = 0; texel < 16; ++texel) {
        const int bitOffset = 3 * texel;
        const int byteIndex = 2 + bitOffset / 8;
        const int shift = bitOffset % 8;
        unsigned value = static_cast<unsigned char>(block[byteIndex]) >> shift;
        if (shift > 5) value |= static_cast<unsigned char>(block[byteIndex + 1]) << (8 - shift);
        const unsigned alpha = alphas[value & 7u];
        unsigned char* out = rgb + 3 * texel;
        for (int i = 0; i < 3; ++i) out[i] = static_cast<unsigned char>((out[i] * alpha) / 255);
    }
}

// First mip only - enough to recognise a texture; uncompressed sections keep their alpha byte, which is
// where mask textures carry their shape (RGB measured constant white in every one inspected) [V-data].
std::vector<unsigned char> decodeMip0(const oa::cdc::Pcd9Info& info, std::span<const std::byte> payload,
                                      int* outChannels) {
    const std::size_t pixels = static_cast<std::size_t>(info.width) * info.height;
    const bool uncompressed = info.tag == static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Uncompressed32);
    const int channels = uncompressed ? 4 : 3;
    *outChannels = channels;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(channels) * pixels);
    const std::byte* data = payload.data() + oa::cdc::kPcd9HeaderBytes;
    if (info.tag == static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Dxt1)) {
        for (std::size_t by = 0; by * 4 < info.height; ++by) {
            for (std::size_t bx = 0; bx * 4 < info.width; ++bx) {
                unsigned char blockRgb[3 * 16];
                decodeBc1(data + (by * ((info.width + 3) / 4) + bx) * 8, blockRgb);
                for (int y = 0; y < 4 && (by * 4 + y) < info.height; ++y) {
                    for (int x = 0; x < 4 && (bx * 4 + x) < info.width; ++x) {
                        const std::size_t src = 3 * ((y * 4) + x);
                        const std::size_t dst = 3 * (((by * 4 + y) * info.width) + bx * 4 + x);
                        for (int c = 0; c < 3; ++c) rgb[dst + c] = blockRgb[src + c];
                    }
                }
            }
        }
    } else if (info.tag == static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Dxt5)) {
        for (std::size_t by = 0; by * 4 < info.height; ++by) {
            for (std::size_t bx = 0; bx * 4 < info.width; ++bx) {
                unsigned char blockRgb[3 * 16];
                decodeBc3(data + (by * ((info.width + 3) / 4) + bx) * 16, blockRgb);
                for (int y = 0; y < 4 && (by * 4 + y) < info.height; ++y) {
                    for (int x = 0; x < 4 && (bx * 4 + x) < info.width; ++x) {
                        const std::size_t src = 3 * ((y * 4) + x);
                        const std::size_t dst = 3 * (((by * 4 + y) * info.width) + bx * 4 + x);
                        for (int c = 0; c < 3; ++c) rgb[dst + c] = blockRgb[src + c];
                    }
                }
            }
        }
    } else if (info.tag == static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Uncompressed32)) {
        // Stored as B8 G8 R8 A8 with RGB constant white; the mask is in alpha, so all four bytes are kept.
        for (std::size_t i = 0; i < pixels; ++i) {
            rgb[4 * i + 0] = static_cast<unsigned char>(data[4 * i + 2]);
            rgb[4 * i + 1] = static_cast<unsigned char>(data[4 * i + 1]);
            rgb[4 * i + 2] = static_cast<unsigned char>(data[4 * i + 0]);
            rgb[4 * i + 3] = static_cast<unsigned char>(data[4 * i + 3]);
        }
    } else {
        return {};
    }
    return rgb;
}

// Minimal PNG writer (filter-0 scanlines, zlib stream) so the tool needs no image library.
bool writePng(const std::string& path, int width, int height, int channels, const unsigned char* pixels) {
    const std::size_t stride = static_cast<std::size_t>(channels) * static_cast<std::size_t>(width);
    std::vector<unsigned char> raw(static_cast<std::size_t>(height) * (stride + 1));
    for (int y = 0; y < height; ++y) {
        const std::size_t rowStart = static_cast<std::size_t>(y) * (stride + 1);
        raw[rowStart] = 0;  // filter: none
        std::memcpy(&raw[rowStart + 1], pixels + static_cast<std::size_t>(y) * stride, stride);
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
        return false;
    }

    const auto put32 = [](std::FILE* file, std::uint32_t value) {
        const unsigned char bytes[4] = {static_cast<unsigned char>(value >> 24),
                                        static_cast<unsigned char>(value >> 16),
                                        static_cast<unsigned char>(value >> 8),
                                        static_cast<unsigned char>(value)};
        std::fwrite(bytes, 1, 4, file);
    };
    const auto chunk = [&](std::FILE* file, const char type[4], const unsigned char* data, std::size_t size) {
        put32(file, static_cast<std::uint32_t>(size));
        std::uint32_t crc = static_cast<std::uint32_t>(crc32(0L, Z_NULL, 0));
        crc = static_cast<std::uint32_t>(crc32(crc, reinterpret_cast<const Bytef*>(type), 4));
        if (size != 0) {
            crc = static_cast<std::uint32_t>(crc32(crc, data, static_cast<uInt>(size)));
        }
        std::fwrite(type, 1, 4, file);
        std::fwrite(data, 1, size, file);
        put32(file, crc);
    };

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, file);
    const auto be32 = [](unsigned char* at, std::uint32_t value) {
        at[0] = static_cast<unsigned char>(value >> 24);
        at[1] = static_cast<unsigned char>(value >> 16);
        at[2] = static_cast<unsigned char>(value >> 8);
        at[3] = static_cast<unsigned char>(value);
    };
    unsigned char ihdr[13];
    be32(ihdr, static_cast<std::uint32_t>(width));
    be32(ihdr + 4, static_cast<std::uint32_t>(height));
    ihdr[8] = 8;   // bit depth
    ihdr[9] = static_cast<unsigned char>(channels == 4 ? 6 : 2);  // truecolour, or with alpha
    std::memset(ihdr + 10, 0, 3);
    chunk(file, "IHDR", ihdr, sizeof(ihdr));
    chunk(file, "IDAT", compressed.data(), static_cast<std::size_t>(compressedSize));
    chunk(file, "IEND", reinterpret_cast<const unsigned char*>(""), 0);
    std::fclose(file);
    return true;
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
        const auto index = findRecord(archive, opt);
        if (!index) {
            return fail("extract needs --hash or --path that matches a record at mask " +
                        std::to_string(opt.mask));
        }

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

    if (opt.command == "drm") {
        const auto index = findRecord(archive, opt);
        if (!index) return fail("drm needs --hash or --path that matches a record");
        const auto data = archive.read(*index, &error);
        if (data.empty() && !error.empty()) return fail(error);

        oa::cdc::DrmFile drm;
        std::string drmError;
        if (!oa::cdc::DrmFile::parse(data, &drm, &drmError)) return fail("DRM: " + drmError);

        const auto& hdr = drm.header();
        std::printf("version=%u sections=%u blockABytes=%u valueD=%u sentinel=%08X image=%zu\n", hdr.version,
                    hdr.numSections, hdr.blockABytes, hdr.valueD, hdr.sentinel, data.size());
        std::printf("payloadBase=%llu (extra region after block A = %lld)\n",
                    static_cast<unsigned long long>(drm.payloadBase()),
                    static_cast<long long>(drm.payloadBase() - 28ull - 20ull * hdr.numSections -
                                            hdr.blockABytes));
        std::printf("%-5s %-4s %-9s %-10s %-7s %-6s %-12s %s\n", "idx", "type", "size", "id", "relocs",
                    "flags", "stored", "payloadOffset");
        for (std::size_t i = 0; i < drm.sections().size(); ++i) {
            const auto& s = drm.sections()[i];
            std::printf("%-5zu %-4u %-9u %-10u %-7u %-6u %-12s %zu\n", i, s.type, s.size, s.id, s.relocCount,
                        s.flags, s.stored ? "yes" : "ABSENT", s.payloadOffset);
        }
        std::fprintf(stderr, "%zu sections, %zu anchor-verified texture payloads\n", drm.sections().size(),
                     drm.anchoredSections());

        if (opt.haveSection) {
            if (opt.section >= drm.sections().size()) return fail("section index out of range");
            const auto payload = drm.payload(drm.sections()[opt.section]);
            for (const auto& r : drm.relocations(drm.sections()[opt.section])) {
                std::fprintf(stderr,
                             "reloc kind=%u target-section=%u typeSpecific=0x%04X site=0x%08llX addend=0x%08llX\n",
                             static_cast<unsigned>(oa::cdc::relocKind(r)),
                             static_cast<unsigned>(oa::cdc::relocSectionIndex(r)), r.typeSpecific,
                             static_cast<unsigned long long>(r.siteOffset),
                             static_cast<unsigned long long>(drm.relocAddend(drm.sections()[opt.section], r)));
            }
            if (opt.out.empty()) {
                std::fwrite(payload.data(), 1, payload.size(), stdout);
                return 0;
            }
            std::FILE* out = std::fopen(opt.out.c_str(), "wb");
            if (out == nullptr) return fail("cannot write " + opt.out);
            const std::size_t written = std::fwrite(payload.data(), 1, payload.size(), out);
            std::fclose(out);
            if (written != payload.size()) return fail("short write to " + opt.out);
            std::fprintf(stderr, "wrote %zu bytes of section %u to %s\n", payload.size(), opt.section,
                         opt.out.c_str());
        }
        return 0;
    }


    if (opt.command == "terrain") {
        const auto index = findRecord(archive, opt);
        if (!index) return fail("terrain needs --hash or --path that matches a record");
        const auto data = archive.read(*index, &error);
        if (data.empty() && !error.empty()) return fail(error);

        oa::cdc::DrmFile drm;
        std::string drmError;
        if (!oa::cdc::DrmFile::parse(data, &drm, &drmError)) return fail("DRM: " + drmError);

        // §15: an oct-tree reaches its strips through the reference in each leaf, and a chain is walked from there - never
        // by scanning payloads for header-shaped bytes. Section 0 holds the level's stride-20 vertex array (§2), so the
        // triangles each tree draws are measured against it: median and p99 longest edge in world units, and the share of
        // edges over 2,000 units, which is where §14's nonsense population lived.
        constexpr std::size_t kRecordBytes = 20;   // docs/formats/GEOMETRY.md §2
        const auto i16At = [](const std::span<const std::byte> p, std::size_t at) {
            return static_cast<std::int32_t>(static_cast<unsigned>(p[at]) | (static_cast<unsigned>(p[at + 1u]) << 8u));
        };
        const oa::cdc::DrmSection& array = drm.sections()[0];
        if (!array.stored || array.size % kRecordBytes != 0u) return fail("section 0 is not a stride-20 vertex array");
        const std::span<const std::byte> vertices = drm.payload(array);
        const auto vertexCount = static_cast<std::uint32_t>(vertices.size() / kRecordBytes);
        const auto edgeOf = [&](std::uint32_t a, std::uint32_t b) {
            double squared = 0.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double delta = static_cast<double>(i16At(vertices, static_cast<std::size_t>(a) * kRecordBytes + 2u * axis)) -
                                     static_cast<double>(i16At(vertices, static_cast<std::size_t>(b) * kRecordBytes + 2u * axis));
                squared += delta * delta;
            }
            return std::sqrt(squared);
        };

        // §20 + sweep-bases3 (GEOMETRY.md §21): a strip record's indices are relative to its material's block of that vertex
        // array, and the unit keeps each block's base in a section of its own. Which same-shaped run is THE table answers from
        // inside this very file: walk every tree once without bases, read the matIdx (header +20) values records name, and take
        // the first decoded table whose length covers them all — findTerrainMaterialBases does exactly that census. Without a
        // covering table every chain is read as if it started at vertex 0 (§14's long-edge population).
        oa::cdc::TerrainMaterialList materials;
        bool haveMaterials = false;
        std::size_t materialSection = 0u;
        {
            std::string why;
            if (oa::cdc::findTerrainMaterialBases(drm, 0u, &materials, &materialSection, &why)) {
                haveMaterials = true;
                std::vector<std::uint32_t> distinct(materials.vertexBase);
                distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
                std::fprintf(stderr, "section %zu: %zu material bases over", materialSection, materials.vertexBase.size());
                for (const std::uint32_t b : distinct) std::fprintf(stderr, " %u", b);
                std::fputc('\n', stderr);
            } else {
                std::fprintf(stderr, "%s\n", why.c_str());   // §20: some units hold no table at all; chains then read absolute (§13)
            }
        }

        std::vector<bool> referenced(vertexCount, false);
        std::uint32_t referencedCount = 0;
        std::size_t trees = 0;
        for (std::size_t i = 0; i < drm.sections().size(); ++i) {
            const auto& nodeSection = drm.sections()[i];
            if (!nodeSection.stored || nodeSection.relocCount == 0) continue;
            std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> references;
            for (const auto& r : drm.relocations(nodeSection)) {
                if (oa::cdc::relocKind(r) != 0) continue;   // §9: node fields are patched by kind-0 entries
                references[static_cast<std::uint32_t>(r.siteOffset)] = {
                    static_cast<std::uint32_t>(oa::cdc::relocSectionIndex(r)), drm.relocAddend(nodeSection, r)};
            }
            oa::cdc::TerrainOctree tree;
            std::string why;
            if (!oa::cdc::decodeTerrainOctree(drm.payload(nodeSection), references, &tree, &why)) {
                std::fprintf(stderr, "section %-4zu: no oct-tree (%s)\n", i, why.c_str());
                continue;
            }
            ++trees;
            std::map<std::uint32_t, std::vector<std::uint32_t>> headsPerSection;
            for (const auto& leaf : tree.leafList) {
                if (leaf.targetSection >= drm.sections().size()) continue;
                headsPerSection[leaf.targetSection].push_back(leaf.offsetInTarget);
            }
            std::vector<std::uint32_t> tris;
            std::uint32_t records = 0, heads = 0, badHeads = 0, brokenLinks = 0, unknownMaterials = 0;
            std::uint32_t low = vertexCount, high = 0u;
            for (const auto& entry : headsPerSection) {
                const auto& strip = drm.sections()[entry.first];
                if (!strip.stored) continue;
                oa::cdc::TerrainStripChain walked;
                std::string chainError;
                if (!oa::cdc::decodeTerrainStripChains(
                        drm.payload(strip), entry.second, vertexCount, &walked, &chainError,
                        haveMaterials ? std::span<const std::uint32_t>(materials.vertexBase)
                                      : std::span<const std::uint32_t>{})) {
                    std::fprintf(stderr, "section %-4u: chain walk skipped (%s)\n", entry.first, chainError.c_str());
                    continue;
                }
                records += walked.records; heads += walked.heads; badHeads += walked.badHeads;
                brokenLinks += walked.brokenLinks; unknownMaterials += walked.unknownMaterials;
                tris.insert(tris.end(), walked.indices.begin(), walked.indices.end());
            }
            std::vector<double> edges;
            edges.reserve(tris.size() / 3u * 3u);
            for (std::size_t t = 0; t + 2 < tris.size(); t += 3u) {
                const std::uint32_t a = tris[t], b = tris[t + 1u], c = tris[t + 2u];
                for (std::uint32_t v : {a, b, c}) { referenced[v] = true; low = std::min(low, v); high = std::max(high, v); }
                edges.push_back(edgeOf(a, b)); edges.push_back(edgeOf(b, c)); edges.push_back(edgeOf(c, a));
            }
            std::sort(edges.begin(), edges.end());
            const auto percentile = [&edges](double fraction) {
                return edges.empty() ? 0.0
                    : edges[std::min(edges.size() - 1u, static_cast<std::size_t>(fraction * static_cast<double>(edges.size())))];
            };
            std::size_t longEdges = 0;
            for (double e : edges) if (e > 2000.0) ++longEdges;
            std::printf("section %-4zu nodes=%-5u leaves=%-5u depth=%-3u | chains: heads=%-5u records=%-5u badHeads=%-3u "
                        "broken=%-3u unknown=%-4u | tris=%-7zu idx %6u..%6u edge p50=%6.0f p99=%7.0f max=%8.0f "
                        "over2000=%.2f%%\n",
                        i, static_cast<unsigned>(tree.nodes), static_cast<unsigned>(tree.leaves),
                        static_cast<unsigned>(tree.maxDepth), heads, records, badHeads, brokenLinks, unknownMaterials,
                        tris.size() / 3u, low, high, percentile(0.5), percentile(0.99),
                        edges.empty() ? 0.0 : edges.back(),
                        edges.empty() ? 0.0 : 100.0 * static_cast<double>(longEdges) / static_cast<double>(edges.size()));
        }
        for (bool r : referenced) if (r) ++referencedCount;
        std::printf("vertices reached by these chains: %u of %u in section 0\n", referencedCount, vertexCount);
        std::fprintf(stderr, "%zu oct-tree section(s)\n", trees);
        return 0;
    }

    if (opt.command == "terrper") {   // §25 measurement: per-tree base-table choice, both rules side by side
        const auto index = findRecord(archive, opt);
        if (!index) return fail("terrper needs --hash or --path that matches a record");
        const auto data = archive.read(*index, &error);
        if (data.empty() && !error.empty()) return fail(error);
        oa::cdc::DrmFile drm;
        std::string drmError;
        if (!oa::cdc::DrmFile::parse(data, &drm, &drmError)) return fail("DRM: " + drmError);
        constexpr std::size_t kRecordBytes = 20;   // §2's stride-20 vertex record
        const auto& array = drm.sections()[0];
        if (!array.stored || array.size % kRecordBytes != 0u) return fail("section 0 is not a stride-20 vertex array");
        const std::span<const std::byte> vertices = drm.payload(array);
        const auto vertexCount = static_cast<std::uint32_t>(vertices.size() / kRecordBytes);
        const auto i16At = [](const std::span<const std::byte> p, std::size_t at) {
            return static_cast<std::int32_t>(static_cast<unsigned>(p[at]) | (static_cast<unsigned>(p[at + 1u]) << 8u));
        };
        const auto edgeOf = [&](std::uint32_t a, std::uint32_t b) {
            double squared = 0.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double delta = static_cast<double>(i16At(vertices, static_cast<std::size_t>(a) * kRecordBytes + 2u * axis)) -
                                     static_cast<double>(i16At(vertices, static_cast<std::size_t>(b) * kRecordBytes + 2u * axis));
                squared += delta * delta;
            }
            return std::sqrt(squared);
        };
        using Rule = oa::cdc::TerrainTableRule;
        for (const auto rule : {Rule::firstCovering, Rule::shortestCovering, Rule::relocNamed, Rule::relocOrFirstCovering, Rule::groupRecord}) {
            std::vector<oa::cdc::TerrainTreeWalk> trees;
            std::string why;
            if (!oa::cdc::decodeTerrainUnit(drm, 0u, rule, &trees, &why)) return fail(why);
            std::printf("%s\n", rule == Rule::firstCovering ? "first-covering rule (§21):" : rule == Rule::shortestCovering ? "shortest-covering rule:" : rule == Rule::relocNamed ? "reloc-named rule (§26):" : rule == Rule::relocOrFirstCovering ? "reloc-or-first-covering hybrid:" : "group-record pairing (§29):");
            for (const auto& walk : trees) {
                std::vector<std::uint32_t> tris;
                std::uint32_t records = 0, heads = 0, badHeads = 0, broken = 0, unknown = 0;
                for (const auto& entry : walk.byPayload) {
                    records += entry.second.records; heads += entry.second.heads;
                    badHeads += entry.second.badHeads; broken += entry.second.brokenLinks;
                    unknown += entry.second.unknownMaterials;
                    tris.insert(tris.end(), entry.second.indices.begin(), entry.second.indices.end());
                }
                std::vector<double> edges;
                edges.reserve(tris.size());
                for (std::size_t t = 0; t + 2 < tris.size(); t += 3u) {
                    const auto a = tris[t], b = tris[t + 1u], c = tris[t + 2u];
                    edges.push_back(edgeOf(a, b)); edges.push_back(edgeOf(b, c)); edges.push_back(edgeOf(c, a));
                }
                std::sort(edges.begin(), edges.end());
                const auto pct = [&edges](double f) {
                    return edges.empty() ? 0.0 : edges[std::min(edges.size() - 1u, static_cast<std::size_t>(f * static_cast<double>(edges.size())))];
                };
                std::size_t longEdges = 0;
                for (double e : edges) if (e > 2000.0) ++longEdges;
                std::uint32_t low = vertexCount, high = 0u;
                for (std::uint32_t v : tris) { low = std::min(low, v); high = std::max(high, v); }
                std::printf("  tree section %-4zu table %4lld refs=%-3u | heads=%-5u records=%-5u badHeads=%-3u broken=%-3u unknown=%-4u "
                            "tris=%-7zu idx %6u..%6u p50=%6.0f p99=%7.0f over2000=%.2f%%\n",
                            walk.nodeSection, static_cast<long long>(walk.tableSection), walk.tableReferences,
                            heads, records, badHeads, broken, unknown, tris.size() / 3u, low, high, pct(0.5), pct(0.99),
                            edges.empty() ? 0.0 : 100.0 * static_cast<double>(longEdges) / static_cast<double>(edges.size()));
            }
        }
        return 0;
    }

    if (opt.command == "namescan") {  // AUDIO.md §8 [V-exe]: FUN_00419a80 chain opens streams as `%s%s.mul` over the prefix table at 0x665090 (`event_triggered\`, `stingers\`, ...) and the fragment `%ssndstrm\` at 0x6523a8 sits in the same pool; type-7 payloads store the joined key, so a record's path is pc-w\ + sndstrm\ + key + .mul
        std::map<std::string, std::pair<std::size_t, std::size_t>> occurrences;  // key -> (units naming it, first record index)
        std::size_t unitsScanned = 0, t7Payloads = 0;
        for (std::size_t i = 0; i < archive.records().size(); ++i) {
            const auto data = archive.read(i, &error);
            if (data.empty()) continue;
            oa::cdc::DrmFile drm;
            std::string drmError;
            error.clear();
            if (!oa::cdc::DrmFile::parse(data, &drm, &drmError)) continue;
            ++unitsScanned;
            for (const auto& section : drm.sections()) {
                if (section.type != 7u || !section.stored) continue;
                const auto payload = drm.payload(section);
                ++t7Payloads;
                // NUL-delimited runs whose whole charset is a path alphabet and that contain a backslash.
                for (std::size_t at = 0; at < payload.size(); ++at) {
                    const auto start = static_cast<unsigned char>(payload[at]);
                    if (!((start >= 'A' && start <= 'Z') || (start >= 'a' && start <= 'z'))) continue;
                    if (at != 0u && static_cast<unsigned char>(payload[at - 1u]) != 0u) continue;
                    bool okRun = true;
                    std::size_t end = at;
                    int backslashes = 0;
                    while (end < payload.size()) {
                        const auto v = static_cast<unsigned char>(payload[end]);
                        if (v == 0u) break;
                        const bool alphaNum = (v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9');
                        const bool sep = v == '_' || v == '.' || v == '-' || v == '\\';
                        if (!alphaNum && !sep) { okRun = false; break; }
                        if (v == '\\') ++backslashes;
                        ++end;
                    }
                    if (!okRun || backslashes == 0 || end >= payload.size()) continue;
                    std::string token;
                    for (std::size_t k = at; k < end; ++k)
                        token.push_back(static_cast<char>(static_cast<unsigned char>(payload[k])));
                    auto& slot = occurrences[token];
                    if (slot.second == 0u && i != 0u) slot.second = i;  // first owning record index, kept stable
                    ++slot.first;
                    at = end;
                }
            }
        }
        std::size_t matchedKeys = 0;
        std::vector<std::string> written;
        for (const auto& [key, seen] : occurrences) {
            const std::string candidate = "pc-w\\sndstrm\\" + key + ".mul";
            written.push_back(candidate);
            if (const auto index = archive.find(candidate, oa::cdc::kMaskAll)) {
                ++matchedKeys;
                printRecord(archive.records()[*index], candidate.c_str());
            } else {
                std::fprintf(stderr, "unresolved key: %s in %zu unit(s), first record index %zu\n", key.c_str(), seen.first, seen.second);
            }
        }
        if (!opt.out.empty()) {
            std::FILE* out = std::fopen(opt.out.c_str(), "wb");
            if (out == nullptr) return fail("cannot write " + opt.out);
            for (const auto& line : written) {
                std::fwrite(line.data(), 1, line.size(), out);
                std::fwrite("\n", 1u, 1u, out);
            }
            std::fclose(out);
        }
        std::fprintf(stderr, "namescan: %zu units scanned, %zu type-7 payloads, %zu distinct keys, matched=%zu unresolved=%zu\n",
                     unitsScanned, t7Payloads, occurrences.size(), matchedKeys, occurrences.size() - matchedKeys);
        return 0;
    }
    if (opt.command == "texture") {
        const auto index = findRecord(archive, opt);
        if (!index) return fail("texture needs --hash or --path that matches a record");

        const auto data = archive.read(*index, &error);
        if (data.empty() && !error.empty()) return fail(error);

        oa::cdc::DrmFile drm;
        std::string drmError;
        if (!oa::cdc::DrmFile::parse(data, &drm, &drmError)) return fail("DRM: " + drmError);

        // --section picks the unit; without it the first section that parses as a PCD9 image is used.
        std::size_t wanted = drm.sections().size();
        oa::cdc::Pcd9Info info;
        std::span<const std::byte> payload;
        std::string texError;
        if (opt.haveSection) {
            if (opt.section >= drm.sections().size()) return fail("section index out of range");
            wanted = opt.section;
            payload = drm.payload(drm.sections()[wanted]);
            if (!oa::cdc::parsePcd9(payload, &info, &texError)) return fail("PCD9: " + texError);
        } else {
            for (std::size_t i = 0; i < drm.sections().size(); ++i) {
                const auto candidate = drm.payload(drm.sections()[i]);
                oa::cdc::Pcd9Info probe;
                std::string ignored;
                if (oa::cdc::parsePcd9(candidate, &probe, &ignored)) {
                    wanted = i;
                    info = probe;
                    payload = candidate;
                    break;
                }
            }
            if (wanted == drm.sections().size()) return fail("no PCD9 section found, pass --section");
        }
        if (opt.out.empty()) return fail("texture needs --out FILE.png");

        int channels = 3;
        const auto pixels = decodeMip0(info, payload, &channels);
        if (pixels.empty()) return fail("unsupported PCD9 format tag");
        if (!writePng(opt.out, info.width, info.height, channels, pixels.data())) {
            return fail("cannot write " + opt.out);
        }
        std::fprintf(stderr, "wrote %ux%u mip 0 of section %zu to %s (tag=%c%c%c%c mips=%u usage=%u)\n",
                     info.width, info.height, wanted, opt.out.c_str(), static_cast<char>(info.tag & 0xFF),
                     static_cast<char>((info.tag >> 8) & 0xFF), static_cast<char>((info.tag >> 16) & 0xFF),
                     static_cast<char>((info.tag >> 24) & 0xFF), info.mipLevels, info.usage);
        return 0;
    }
    return fail("unknown command '" + opt.command + "'");
}
