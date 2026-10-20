#include "cdc/Texture.hpp"

#include <cstdio>

namespace oa::cdc {
namespace {

std::uint32_t readU32(std::span<const std::byte> image, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(image[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 1])) << 8u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 2])) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 3])) << 24u);
}

std::uint16_t readU16(std::span<const std::byte> image, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(image[at]) |
                                      (static_cast<unsigned char>(image[at + 1]) << 8));
}

std::string hex(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%X", value);
    return buffer;
}

// One block row/column count never drops below one, which is what makes the two smallest levels of a
// 512-wide chain cost a single block each - matching lara.drm's 512x512 DXT1 payload of 174776 bytes.
std::uint32_t blocksAt(std::uint16_t dimension, std::uint32_t level) {
    const std::uint32_t d = static_cast<std::uint32_t>(dimension) >> level;
    return (d < 4u) ? 1u : (d + 3u) / 4u;
}

std::uint32_t dimAt(std::uint16_t dimension, std::uint32_t level) {
    const std::uint32_t d = static_cast<std::uint32_t>(dimension) >> level;
    return d == 0u ? 1u : d;
}

std::uint64_t blockBytesOf(std::uint32_t tag) {
    if (tag == static_cast<std::uint32_t>(Pcd9Format::Uncompressed32)) return 4u;
    return tag == static_cast<std::uint32_t>(Pcd9Format::Dxt5) ? 16u : 8u;
}

}  // namespace

std::uint64_t pcd9MipChainBytes(const Pcd9Info& info) {
    if (info.mipLevels == 0) return 0;
    std::uint64_t total = 0;
    for (std::uint32_t level = 0; level < info.mipLevels; ++level) {
        const std::uint32_t w = dimAt(info.width, level);
        const std::uint32_t h = dimAt(info.height, level);
        if (info.tag == static_cast<std::uint32_t>(Pcd9Format::Uncompressed32)) {
            total += static_cast<std::uint64_t>(w) * h * 4u;
        } else {
            const std::uint64_t perBlock = blockBytesOf(info.tag);
            total += static_cast<std::uint64_t>(blocksAt(info.width, level)) *
                     blocksAt(info.height, level) * perBlock;
        }
    }
    return total;
}

bool parsePcd9(std::span<const std::byte> payload, Pcd9Info* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("parsePcd9 needs an output object");
    if (payload.size() < kPcd9HeaderBytes) return fail("texture payload is shorter than 24 bytes");
    if (readU32(payload, 0) != 0x39444350u) {
        return fail("texture payload does not start with PCD9");
    }

    Pcd9Info info;
    info.tag = readU32(payload, 4);
    info.payloadBytes = readU32(payload, 8);
    info.width = readU16(payload, 16);
    info.height = readU16(payload, 18);
    info.bitsPerPixel = static_cast<unsigned char>(payload[20]);
    const unsigned storedLevels = static_cast<unsigned>(payload[21]) + 1u;
    info.usage = readU16(payload, 22);

    // No chain of that many levels fits a u16 extent, so the input is corrupt.
    if (storedLevels > 32u) return fail("implausible mip count " + std::to_string(storedLevels));
    info.mipLevels = static_cast<std::uint8_t>(storedLevels);

    if (info.tag != static_cast<std::uint32_t>(Pcd9Format::Uncompressed32) &&
        info.tag != static_cast<std::uint32_t>(Pcd9Format::Dxt1) &&
        info.tag != static_cast<std::uint32_t>(Pcd9Format::Dxt5)) {
        return fail("unknown PCD9 format tag " + hex(info.tag));
    }
    if (info.width == 0 || info.height == 0) return fail("texture extent is zero");
    if (info.tag == static_cast<std::uint32_t>(Pcd9Format::Uncompressed32) && info.bitsPerPixel != 32u) {
        return fail("uncompressed texture declares " + std::to_string(info.bitsPerPixel) +
                    " bits per pixel, only 32 is known");
    }
    if (payload.size() - kPcd9HeaderBytes != info.payloadBytes) {
        return fail("header declares " + std::to_string(info.payloadBytes) +
                    " payload bytes but the section holds " +
                    std::to_string(payload.size() - kPcd9HeaderBytes));
    }

    // The chain total is what the engine's own size arithmetic has to agree with; a mismatch would put
    // every mip after the first at the wrong offset. Verified against all 71 textures of lara.drm.
    const std::uint64_t chain = pcd9MipChainBytes(info);
    if (chain != info.payloadBytes) {
        return fail(std::to_string(info.mipLevels) + " mips of " + std::to_string(info.width) + "x" +
                    std::to_string(info.height) + " need " + std::to_string(chain) +
                    " bytes, the header declares " + std::to_string(info.payloadBytes));
    }

    *out = info;
    return true;
}

}  // namespace oa::cdc
