#pragma once
// PCD9 texture payloads of DRM v14 units (docs/formats/TEXTURES.md).
//
// The 24-byte header and the tag set were measured across all 71 textures of lara.drm; tra.exe
// validates the magic and reads width/height/mip count from the same offsets. Block formats are kept
// as-is: BC1/BC3 map onto Vulkan formats directly, so the engine never decodes on the CPU.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oa::cdc {

inline constexpr std::size_t kPcd9HeaderBytes = 24;

enum class Pcd9Format : std::uint32_t {
    Uncompressed32 = 0x15,   // 'PCD9' + 21: ARGB8888, the tag ZenHAX reported as "version 21" [S]
    Dxt1 = 0x31545844,       // 'DXT1' little-endian
    Dxt5 = 0x35545844,       // 'DXT5' little-endian
};

struct Pcd9Info {
    std::uint32_t tag = 0;      // one of Pcd9Format
    std::uint32_t payloadBytes = 0;   // everything after the 24-byte header
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t bitsPerPixel = 0;    // 32 for Uncompressed32, 0 for block formats
    std::uint8_t mipLevels = 0;       // stored as levels-1
    std::uint16_t usage = 0;          // engine-side flag word (semantics unproven)
};

// Reads and validates one texture payload: magic, known tag, the declared payload length matching
// what is actually present, and a mip chain whose byte total equals that length. Returns false with
// *error filled instead of accepting an image whose blocks could not be uploaded intact.
bool parsePcd9(std::span<const std::byte> payload, Pcd9Info* out, std::string* error = nullptr);

// Expected byte count of a full mip chain for the given format/extent/level count; 0 when the level
// count cannot describe a coherent chain. Exposed because both validation and the upload path need it.
std::uint64_t pcd9MipChainBytes(const Pcd9Info& info);

// One level of a packed mip chain: byte offset from the start of the payload plus its length. An uploader
// needs these because vkCmdCopyBufferToImage takes one region per mip level.
struct Pcd9LevelRegion {
    std::uint64_t offset = 0;
    std::uint32_t bytes = 0;
};

// Regions in payload order (largest level first); totals agree with pcd9MipChainBytes.
std::vector<Pcd9LevelRegion> pcd9LevelRegions(const Pcd9Info& info);

}  // namespace oa::cdc
