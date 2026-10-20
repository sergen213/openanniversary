#pragma once
// Geometry payloads: the general (type-0) sections that hold a level's triangles
// (docs/formats/GEOMETRY.md §1, §2, §6).
//
// The layout is established from the shipped install, not from community offsets: in unit 8baf540e the largest
// type-0 section is 2,981,540 bytes and divides exactly by nothing but 4 and 20; taking records 20 bytes apart
// and reading every three as one triangle gives a median longest edge of 146 world units against 10,898 for
// random vertex pairs [V-data]. Field values below are the census over all 149,077 vertices of that section.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oa::cdc {

inline constexpr std::size_t kMeshVertexBytes = 20;

// One vertex: position in world units, a modulate colour and a texture coordinate. `flag` is 1 and `pad` is 0
// in every vertex of that section [V-data]; neither is required to decode, because other units may use the
// spare words. The alpha byte is mostly 0x80 and is not treated as transparency anywhere yet.
struct MeshVertex {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t z = 0;
    std::uint16_t flag = 0;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;
    std::int16_t texS = 0;
    std::int16_t texT = 0;
    std::uint32_t pad = 0;
};

static_assert(sizeof(MeshVertex) == kMeshVertexBytes);

struct MeshBounds {
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t triangles = 0;
};

// Decodes an unindexed triangle run: three vertices per triangle, no index buffer. The payload must be a whole
// number of records and hold at least one triangle; nothing else about the bytes is assumed.
bool decodeMesh(std::span<const std::byte> payload, std::vector<MeshVertex>* out, std::string* error = nullptr);

// Axis-aligned bounds plus the triangle count, used to place a camera around a mesh.
MeshBounds meshBounds(const std::vector<MeshVertex>& vertices);

}  // namespace oa::cdc
