#include "cdc/Drm.hpp"

#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace oa::cdc {
namespace {

std::uint32_t readU32(std::span<const std::byte> image, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(image[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 1])) << 8u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 2])) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 3])) << 24u);
}

// Bytes one relocation table occupies. The count lives in bits 8-31 of the packed field for v14;
// Underworld's v19 stores bytes instead (docs/formats/DRM.md §2.1). The size-0 tail entry is not a
// count: treating it as one makes generalbank.drm over-consume by 56 bytes and uigeneral.drm by 32,
// while ignoring it closes both byte-exactly with every texture magic in place [V-data].
std::uint64_t relocBytes(const DrmSection& s) {
    if (s.size == 0 && s.type == kDrmTailType) return 0;
    return static_cast<std::uint64_t>(s.relocCount) * 8u;
}

std::string hex(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

}  // namespace

bool DrmFile::parse(std::span<const std::byte> image, DrmFile* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("parse needs an output object");
    if (image.size() < kDrmHeaderBytes) return fail("unit image is shorter than the 28-byte header");

    DrmFile parsed;
    parsed.image_ = image;
    parsed.header_.version = readU32(image, 0);
    parsed.header_.numSections = readU32(image, 4);
    parsed.header_.blockABytes = readU32(image, 8);
    parsed.header_.zero1 = readU32(image, 12);
    parsed.header_.valueD = readU32(image, 16);
    parsed.header_.zero2 = readU32(image, 20);
    parsed.header_.sentinel = readU32(image, 24);

    if (parsed.header_.version != kDrmVersionAnniversary) {
        return fail("unsupported DRM version " + std::to_string(parsed.header_.version) +
                    ", this parser reads Anniversary v14");
    }
    const std::uint64_t count = parsed.header_.numSections;
    if (count == 0) return fail("unit declares no sections");
    const std::uint64_t tableEnd = kDrmHeaderBytes + count * kDrmSectionEntryBytes;
    if (tableEnd > image.size()) {
        return fail("section table of " + std::to_string(count) + " entries ends at " +
                    hex(tableEnd) + ", past the end of the " + std::to_string(image.size()) +
                    "-byte image");
    }

    parsed.sections_.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::size_t at = kDrmHeaderBytes + static_cast<std::size_t>(i) * kDrmSectionEntryBytes;
        DrmSection s;
        s.size = readU32(image, at);
        s.type = static_cast<unsigned char>(image[at + 4]);
        s.flags = static_cast<unsigned char>(image[at + 5]);
        s.versionId = static_cast<std::uint16_t>(readU32(image, at + 6) & 0xFFFFu);
        s.packed = readU32(image, at + 8);
        s.id = readU32(image, at + 12);
        s.specMask = readU32(image, at + 16);
        s.relocCount = (s.size == 0 && s.type == kDrmTailType) ? 0u : (s.packed >> 8u);
        parsed.sections_.push_back(s);
    }

    // The archive stores relocation table then payload per section, in table order, without padding.
    // Subtracting that total from the image size locates the first payload, and every texture magic
    // that lands on PCD9 confirms it. Level units break that arithmetic: pc-w-hash 8baf540e declares a
    // trailing entry whose 1288 bytes are not in the image, so the subtraction shifts the base and
    // misanchors every one of its 36 textures [V-data]. Re-solve from an actual magic and drop a suffix
    // of table entries as unstored until the walk closes byte-exactly again.
    const std::uint64_t kPcd9 = 0x39444350u;  // "PCD9"

    std::uint64_t payloadTotal = 0;
    for (const DrmSection& s : parsed.sections_) {
        payloadTotal += static_cast<std::uint64_t>(s.size) + relocBytes(s);
        if (payloadTotal > image.size()) {
            return fail("section payloads and relocation tables need " + hex(payloadTotal) +
                        " bytes, more than the whole image");
        }
    }

    // Offsets of every PCD9 in the image, used as alternative anchors.
    std::vector<std::size_t> magics;
    for (std::size_t at = 0; at + 4 <= image.size() && magics.size() < 8192u; ++at) {
        if (readU32(image, at) == kPcd9) magics.push_back(at);
    }

    // Places payloads and relocation tables for one candidate base, verifies every texture magic, and
    // marks a suffix of table entries unstored so the walk ends exactly at the image end.
    const bool hasTextureSection = std::any_of(parsed.sections_.begin(), parsed.sections_.end(),
                                              [](const DrmSection& s) { return s.type == 5; });
    std::string why;  // last rejection, reported when no candidate base works
    const auto tryBase = [&](std::uint64_t base) -> bool {
        if (base < tableEnd || base > image.size()) {
            why = "candidate base " + hex(base) + " overlaps the section table or passes the end of the image";
            return false;
        }

        // Close the walk against the end of the image, marking a suffix of entries unstored.
        std::uint64_t endPos = base;
        for (const DrmSection& s : parsed.sections_) endPos += static_cast<std::uint64_t>(s.size) + relocBytes(s);
        std::size_t dropFrom = parsed.sections_.size();
        while (endPos > image.size() && dropFrom > 0) {
            --dropFrom;
            const DrmSection& s = parsed.sections_[dropFrom];
            endPos -= static_cast<std::uint64_t>(s.size) + relocBytes(s);
        }
        if (endPos != image.size()) {
            why = "the walk from base " + hex(base) + " ends at " + hex(endPos) + ", not the end of the " +
                  std::to_string(image.size()) + "-byte image";
            return false;
        }

        std::size_t anchored = 0;
        std::uint64_t pos = base;
        for (std::size_t i = 0; i < parsed.sections_.size(); ++i) {
            DrmSection& s = parsed.sections_[i];
            s.stored = i < dropFrom;
            if (!s.stored) {
                s.payloadOffset = 0;
                continue;
            }
            const std::uint64_t rb = relocBytes(s);
            s.payloadOffset = static_cast<std::size_t>(pos + rb);
            pos += s.size + rb;
            if (s.type == 5) {
                if (!(s.payloadOffset + 4 <= image.size() && readU32(image, s.payloadOffset) == kPcd9)) {
                    why = "texture section " + std::to_string(i) + " at " + hex(s.payloadOffset) +
                          " does not start with PCD9";
                    return false;
                }
                ++anchored;
            }
        }
        // A unit with texture payloads is validated structurally by every magic landing where the
        // directory says, so hdr[2] cannot veto it: pc-w-hash F2197DE6 anchors all 9 of its textures and
        // closes exactly at the image end from base 416, yet its pre-payload region is 4 bytes below the
        // 132 bytes that field declares [V-data]. Units without any magic to anchor on still need that
        // check, because closure alone there proves nothing about where payloads begin.
        if (!hasTextureSection && base < tableEnd + parsed.header_.blockABytes) {
            const auto room = static_cast<std::int64_t>(base) - static_cast<std::int64_t>(tableEnd);
            why = "pre-payload region is " + hex(static_cast<std::uint64_t>(room)) + " bytes, below the " +
                  std::to_string(parsed.header_.blockABytes) + " bytes the header declares";
            return false;
        }
        parsed.anchored_ = anchored;
        parsed.payloadBase_ = static_cast<std::size_t>(base);
        return true;
    };

    // The first entry with a magic fixes the base directly: everything stored ahead of it sits at its
    // normal distance, and that distance includes the relocation table kept before its own payload.
    if (!tryBase(static_cast<std::uint64_t>(image.size()) - payloadTotal)) {
        std::uint64_t rel = 0;
        bool haveRel = false;
        for (const DrmSection& s : parsed.sections_) {
            rel += relocBytes(s);
            if (s.type == 5) {
                haveRel = true;
                break;
            }
            rel += static_cast<std::uint64_t>(s.size);
        }
        bool solved = false;
        for (std::size_t magic : magics) {
            if (!haveRel || magic < rel) continue;
            DrmFile probe = parsed;  // tryBase mutates offsets, so restart from the clean table
            if (tryBase(magic - rel)) {
                solved = true;
                break;
            }
            parsed = probe;
        }
        if (!solved) {
            return fail("no payload base anchors every texture section on PCD9 and closes the " +
                        std::to_string(parsed.sections_.size()) + "-entry table at the end of the " +
                        std::to_string(image.size()) + "-byte image (declared bytes " + hex(payloadTotal) +
                        ", magics found " + std::to_string(magics.size()) + "); last reason: " + why);
        }
    }
    *out = std::move(parsed);
    return true;
}

std::vector<DrmReloc> DrmFile::relocations(const DrmSection& section) const {
    std::vector<DrmReloc> out;
    if (!section.stored || section.relocCount == 0) return out;
    const std::size_t tableStart = section.payloadOffset - relocBytes(section);
    out.reserve(section.relocCount);
    for (std::uint32_t i = 0; i < section.relocCount; ++i) {
        const std::size_t at = tableStart + static_cast<std::size_t>(i) * 8u;
        const std::uint32_t word = readU32(image_, at);
        out.push_back({static_cast<std::uint16_t>(word & 0xFFFFu), static_cast<std::uint16_t>(word >> 16u),
                       readU32(image_, at + 4)});
    }
    return out;
}

std::uint32_t DrmFile::relocAddend(const DrmSection& section, const DrmReloc& reloc) const {
    if (!section.stored) return 0;
    const std::size_t at = section.payloadOffset + reloc.siteOffset;
    if (at + 4 > image_.size()) return 0;
    return readU32(image_, at);
}

}  // namespace oa::cdc
