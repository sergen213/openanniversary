// oa-asset: inspect and extract the cdc bigfile archive of a Legend-era install (read-only).
//
//   oa-asset <game-dir> list [--mask HEX]                 dump the record table
//   oa-asset <game-dir> hash <path>...                    hashes the game would search for
//   oa-asset <game-dir> resolve <paths.txt>               pair candidate paths with the record table
//   oa-asset <game-dir> extract --hash HEX [--mask HEX] --out FILE
//   oa-asset <game-dir> extract --path "pc-w\lara.drm" --out FILE
//   oa-asset <game-dir> drm --path PATH | --hash HEX [--section N --out FILE]
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
        error = "usage: oa-asset <game-dir> <list|hash|resolve|extract|drm|texture|terrain> [options]";
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

        // A strip's u16 values address some stride-20 vertex payload of this unit. §2 established section 0 as the
        // level's array, but that pairing has to be measured, not assumed: coherence decides it. Two indices next to
        // each other inside one strip are neighbours on one surface sheet, so their positions must sit close together.
        // The number printed per tree is the root-mean-square longest triangle edge as a percentage of the candidate
        // payload's own span; read against the wrong array an index lands anywhere and that figure goes to tens of
        // percent instead of around one.
        constexpr std::size_t kRecordBytes = 20;   // docs/formats/GEOMETRY.md §2
        const auto i16At = [](const std::span<const std::byte> p, std::size_t at) {
            return static_cast<std::int32_t>(static_cast<unsigned>(p[at]) | (static_cast<unsigned>(p[at + 1u]) << 8u));
        };

        struct Coherence {
            bool ok = false;
            double rmsPercent = 0.0;
            std::size_t longEdges = 0;   // edges longer than 1% of the payload's span
            std::uint32_t records = 0;
        };

        const auto coherenceOf = [&](std::size_t arraySection, const std::vector<std::uint32_t>& tris) {
            Coherence out;
            const oa::cdc::DrmSection& candidate = drm.sections()[arraySection];
            if (!candidate.stored || candidate.type != 0) return out;
            const auto payload = drm.payload(candidate);
            if (payload.size() < 2u * kRecordBytes) return out;
            out.records = static_cast<std::uint32_t>(payload.size() / kRecordBytes);
            std::int32_t low[3] = {32767, 32767, 32767};
            std::int32_t high[3] = {-32768, -32768, -32768};
            for (std::uint32_t r = 0; r < out.records; ++r) {
                for (int axis = 0; axis < 3; ++axis) {
                    const std::int32_t value = i16At(payload, static_cast<std::size_t>(r) * kRecordBytes + 2u * axis);
                    low[axis] = std::min(low[axis], value);
                    high[axis] = std::max(high[axis], value);
                }
            }
            double spanSquared = 0.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double extent = static_cast<double>(high[axis]) - static_cast<double>(low[axis]);
                spanSquared += extent * extent;
            }
            if (!(spanSquared > 1.0)) return out;   // a payload that is one point cannot be an array to measure against
            double sum = 0.0;
            std::size_t counted = 0;
            for (std::size_t t = 0; t + 2 < tris.size(); t += 3u) {
                const std::uint32_t corner[3] = {tris[t], tris[t + 1u], tris[t + 2u]};
                if (corner[0] >= out.records || corner[1] >= out.records || corner[2] >= out.records) continue;
                double longest = 0.0;
                for (int edge = 0; edge < 3; ++edge) {
                    const std::size_t first = static_cast<std::size_t>(corner[edge]) * kRecordBytes;
                    const std::size_t second = static_cast<std::size_t>(corner[(edge + 1) % 3]) * kRecordBytes;
                    double squared = 0.0;
                    for (int axis = 0; axis < 3; ++axis) {
                        const double delta = static_cast<double>(i16At(payload, first + 2u * axis)) -
                                             static_cast<double>(i16At(payload, second + 2u * axis));
                        squared += delta * delta;
                    }
                    longest = std::max(longest, squared);
                }
                sum += longest / spanSquared;
                if (longest > spanSquared / 10000.0) ++out.longEdges;
                ++counted;
            }
            if (counted == 0) return out;
            out.ok = true;
            out.rmsPercent = 100.0 * std::sqrt(sum / static_cast<double>(counted));
            return out;
        };

        // §9's segmentation probe, kept here rather than in the library because its model is refuted for most
        // payloads (§10): it cuts a payload at the offsets leaves resolve to and reads each block as u16 indices.
        // A block that does not divide into whole in-range indices yields nothing, which is what `rejected` counts.
        const auto runsSegmentedAt = [&](const std::span<const std::byte> payload,
                                        std::vector<std::uint32_t> starts) {
            std::sort(starts.begin(), starts.end());
            starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
            std::vector<std::uint32_t> tris;
            for (std::size_t block = 0; block < starts.size(); ++block) {
                const std::uint32_t begin = starts[block];
                const std::uint32_t end = block + 1u < starts.size()
                                              ? starts[block + 1u]
                                              : static_cast<std::uint32_t>(payload.size());
                if (begin >= end || end > payload.size()) return std::vector<std::uint32_t>{};
                const std::size_t count = (end - begin) / 2u;
                for (std::size_t i = 0; i < count; ++i) {
                    if (i16At(payload, begin + 2u * i) >= 0x10000) return std::vector<std::uint32_t>{};
                }
                for (std::size_t i = 2; i < count; ++i) {
                    const std::uint32_t a = static_cast<std::uint32_t>(i16At(payload, begin + 2u * (i - 2u)));
                    const std::uint32_t b = static_cast<std::uint32_t>(i16At(payload, begin + 2u * (i - 1u)));
                    const std::uint32_t c = static_cast<std::uint32_t>(i16At(payload, begin + 2u * i));
                    if (a == b || b == c || a == c) continue;   // degenerate restart between strips, §9
                    tris.insert(tris.end(), {a, b, c});
                }
            }
            return tris;
        };


        // §10: what those payloads do hold. A record start is accepted when its header is sane, its u16 index run stays
        // inside section 0's stride-20 array (§2), and the triangles it draws are coherent against that array - RMS
        // longest edge under 1% of the array's span. Accepting a record claims the bytes it spans, so starts never
        // overlap. The header follows prior art tr7/game/terrain.h:46 TerrainTextureStripInfo, 44 bytes: vertexCount at
        // +0, matIdx (i32) at +20 and nextTexture (u32) at +40; the indices start at +44.
        const auto u32At = [](const std::span<const std::byte> p, std::size_t at) {
            return static_cast<std::uint32_t>(static_cast<unsigned>(p[at])) |
                   (static_cast<std::uint32_t>(static_cast<unsigned>(p[at + 1u])) << 8u) |
                   (static_cast<std::uint32_t>(static_cast<unsigned>(p[at + 2u])) << 16u) |
                   (static_cast<std::uint32_t>(static_cast<unsigned>(p[at + 3u])) << 24u);
        };
        constexpr std::size_t kStripHeaderBytes = 44;
        struct StripRecord {
            std::uint32_t offset = 0;
            std::uint32_t vertexCount = 0;
            std::int32_t matIdx = 0;
            std::uint32_t nextTexture = 0;
        };
        const std::uint32_t kVertexLimit = static_cast<std::uint32_t>(drm.sections()[0].size / kRecordBytes);
        std::vector<bool> referenced(kVertexLimit, false);
        std::uint32_t referencedCount = 0;
        std::map<std::size_t, std::vector<StripRecord>> stripRecords;   // section index -> its records, in offset order
        for (std::size_t s = 1; s < drm.sections().size(); ++s) {
            const auto& section = drm.sections()[s];
            if (!section.stored || section.type != 0 || section.size < kStripHeaderBytes + 6u) continue;
            const auto payload = drm.payload(section);
            std::vector<StripRecord> records;
            for (std::size_t off = 0; off + kStripHeaderBytes <= payload.size(); off += 2u) {
                const std::uint32_t vc = u32At(payload, off);
                if (vc < 3 || vc > 8192) continue;
                const std::size_t end = off + kStripHeaderBytes + 2u * vc;
                if (end > payload.size()) continue;
                const std::size_t base = off + kStripHeaderBytes;
                bool inRange = true;
                for (std::uint32_t i = 0; i < vc && inRange; ++i) {
                    inRange = static_cast<std::uint32_t>(i16At(payload, base + 2u * i)) < kVertexLimit;
                }
                if (!inRange) continue;
                std::vector<std::uint32_t> tris;
                for (std::uint32_t i = 2; i < vc; ++i) {
                    const std::uint32_t a = static_cast<std::uint32_t>(i16At(payload, base + 2u * (i - 2u)));
                    const std::uint32_t b = static_cast<std::uint32_t>(i16At(payload, base + 2u * (i - 1u)));
                    const std::uint32_t c = static_cast<std::uint32_t>(i16At(payload, base + 2u * i));
                    if (a == b || b == c || a == c) continue;   // degenerate restart between strips, §9
                    tris.insert(tris.end(), {a, b, c});
                }
                const Coherence coherence = coherenceOf(0, tris);
                if (!coherence.ok || coherence.rmsPercent >= 1.0) continue;
                records.push_back({static_cast<std::uint32_t>(off), vc, static_cast<std::int32_t>(u32At(payload, off + 20u)),
                                   u32At(payload, off + 40u)});
                for (std::uint32_t i = 0; i < vc; ++i) {
                    if (const std::uint32_t v = static_cast<std::uint32_t>(i16At(payload, base + 2u * i)); !referenced[v]) {
                        referenced[v] = true;
                        ++referencedCount;
                    }
                }
                off = end - 2u;   // the loop step lands on `end`, the byte after this record
            }
            if (!records.empty()) stripRecords.emplace(s, std::move(records));
        }
        std::size_t totalRecords = 0;
        std::size_t chained = 0;
        for (const auto& entry : stripRecords) {
            for (const StripRecord& r : entry.second) {
                ++totalRecords;
                if (std::any_of(entry.second.begin(), entry.second.end(),
                                [&](const StripRecord& o) { return o.offset == r.nextTexture; })) {
                    ++chained;
                }
            }
        }
        std::size_t leafTotal = 0;
        std::size_t leafRecordHits = 0;
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
            std::map<std::uint32_t, std::vector<std::uint32_t>> startsPerSection;
            for (const auto& leaf : tree.leafList) {
                if (leaf.targetSection >= drm.sections().size()) continue;
                startsPerSection[leaf.targetSection].push_back(leaf.offsetInTarget);
                const auto found = stripRecords.find(leaf.targetSection);
                ++leafTotal;
                if (found != stripRecords.end() &&
                    std::any_of(found->second.begin(), found->second.end(),
                                [&](const StripRecord& r) { return r.offset == leaf.offsetInTarget; })) {
                    ++leafRecordHits;   // a leaf that lands on a real record start, §10
                }
            }
            std::vector<std::uint32_t> tris;
            std::size_t rejected = 0;
            for (const auto& entry : startsPerSection) {
                const auto& strip = drm.sections()[entry.first];
                if (!strip.stored) {
                    ++rejected;
                    continue;
                }
                const std::vector<std::uint32_t> run = runsSegmentedAt(drm.payload(strip), entry.second);
                if (run.empty()) {
                    ++rejected;   // the block holds no whole u16 runs
                    continue;
                }
                tris.insert(tris.end(), run.begin(), run.end());
            }
            if (tris.empty()) {
                std::printf("section %-4zu nodes=%-5u leaves=%-5u depth=%-3u rejected=%-2zu has no usable strip runs\n", i,
                            static_cast<unsigned>(tree.nodes), static_cast<unsigned>(tree.leaves),
                            static_cast<unsigned>(tree.maxDepth), rejected);
                ++trees;
                continue;
            }
            std::uint32_t maxIndex = 0;
            for (const std::uint32_t value : tris) maxIndex = std::max(maxIndex, value);
            const Coherence baseline = coherenceOf(0, tris);   // the array §2 established
            Coherence best = baseline;
            std::size_t bestSection = 0;
            for (std::size_t c = 1; c < drm.sections().size(); ++c) {
                if (!drm.sections()[c].stored || drm.sections()[c].type != 0) continue;
                if (drm.sections()[c].size / kRecordBytes <= maxIndex) continue;   // too small to hold these indices
                const Coherence trial = coherenceOf(c, tris);
                if (trial.ok && (!best.ok || trial.rmsPercent < best.rmsPercent)) {
                    best = trial;
                    bestSection = c;
                }
            }
            std::printf("section %-4zu nodes=%-5u leaves=%-5u depth=%-3u maxIndex=%-6u triangles=%-7zu rejected=%-2zu "
                        "| section 0 rms=%.2f%% | best: section %-4zu records=%-6u rms=%.2f%% longEdges>1%%=%zu\n",
                        i, static_cast<unsigned>(tree.nodes), static_cast<unsigned>(tree.leaves),
                        static_cast<unsigned>(tree.maxDepth), maxIndex, tris.size() / 3u, rejected,
                        baseline.ok ? baseline.rmsPercent : -1.0, bestSection, best.records,
                        best.ok ? best.rmsPercent : -1.0, best.longEdges);
            ++trees;
        }
        std::printf("strip records (GEOMETRY.md 10): %zu payload(s) hold %zu record(s), %zu chain to another record "
                    "start; they reference %u/%u vertices of section 0\n", stripRecords.size(), totalRecords, chained,
                    referencedCount, kVertexLimit);
        std::printf("leaf addends landing on a record start: %zu of %zu\n", leafRecordHits, leafTotal);
        std::fprintf(stderr, "%zu oct-tree section(s)\n", trees);
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
