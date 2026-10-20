#include "cdc/Drm.hpp"

#include <cstdio>
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
    // Subtracting that total from the image size locates the first payload; if the arithmetic does
    // not close, the layout assumption is wrong for this unit and guessing would corrupt content.
    std::uint64_t payloadTotal = 0;
    for (const DrmSection& s : parsed.sections_) {
        payloadTotal += static_cast<std::uint64_t>(s.size) + relocBytes(s);
        if (payloadTotal > image.size()) {
            return fail("section payloads and relocation tables need " + hex(payloadTotal) +
                        " bytes, more than the whole image");
        }
    }
    const std::uint64_t base = static_cast<std::uint64_t>(image.size()) - payloadTotal;
    if (base < tableEnd + parsed.header_.blockABytes) {
        return fail("pre-payload region is " + hex(base - tableEnd) + " bytes, below the " +
                    std::to_string(parsed.header_.blockABytes) + " bytes the header declares");
    }
    parsed.payloadBase_ = static_cast<std::size_t>(base);

    std::uint64_t pos = base;
    std::size_t anchored = 0;
    for (std::size_t i = 0; i < parsed.sections_.size(); ++i) {
        DrmSection& s = parsed.sections_[i];
        const std::uint64_t rb = relocBytes(s);
        s.payloadOffset = static_cast<std::size_t>(pos + rb);
        pos += s.size + rb;
        if (s.type == 5) {
            ++anchored;
            const bool magic = s.payloadOffset + 4 <= image.size() && readU32(image, s.payloadOffset) == 0x39444350u;
            if (!magic) {
                return fail("texture section " + std::to_string(i) + " at " + hex(s.payloadOffset) +
                            " does not start with PCD9");
            }
        }
    }
    parsed.anchored_ = anchored;
    *out = std::move(parsed);
    return true;
}

std::vector<DrmReloc> DrmFile::relocations(const DrmSection& section) const {
    const std::size_t tableStart = section.payloadOffset - relocBytes(section);
    std::vector<DrmReloc> out;
    out.reserve(section.relocCount);
    for (std::uint32_t i = 0; i < section.relocCount; ++i) {
        const std::size_t at = tableStart + static_cast<std::size_t>(i) * 8u;
        out.push_back({readU32(image_, at), readU32(image_, at + 4)});
    }
    return out;
}

}  // namespace oa::cdc
