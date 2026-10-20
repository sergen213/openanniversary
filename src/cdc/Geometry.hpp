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
#include <map>
#include <span>
#include <string>
#include <vector>

namespace oa::cdc {

inline constexpr std::size_t kMeshVertexBytes = 20;

// One vertex: position in world units, a modulate colour and a texture coordinate. `flag` is 1 and `pad` is 0
// in every vertex of that section [V-data]; neither is required to decode, because other units may use the
// spare words. The alpha byte is mostly 0x80 and is not treated as transparency anywhere yet. cdcEngine tr7 names
// the fourth short `w` and the last four bytes `bend`/`bendindex` (`tr7/game/terrain.h:129`, see GEOMETRY.md §8);
// the names here are kept because nothing measured yet gives those fields a meaning.
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

// A level unit's terrain oct-tree (GEOMETRY.md §9), one payload of nodes:
//   +0  four floats - bounding sphere centre and radius
//   +16 u32 reference, used only by leaves; the archive patches it through a relocation entry, so the resolved
//       target arrives from the caller as (section index, offset inside that section's payload)
//   +20 s32 child count, 0-8 observed
//   +24 that many u32 self-relative byte offsets to child nodes, ascending
// A full eight-child branch takes 64 bytes and a leaf is padded to 32; the walk from offset 0 over unit 8baf540e
// section 86 visits 623 nodes with no anomalies [V-data].
// What a leaf's reference actually addresses is still unresolved (§10): these payloads do hold 44-byte strip headers
// followed by u16 index runs into the stride-20 array, but those records do not start where the leaves point.
struct TerrainLeaf {
    std::uint32_t nodeOffset = 0;      // where in this payload the leaf node starts
    std::uint32_t targetSection = 0;   // section whose payload the reference resolves into
    std::uint32_t offsetInTarget = 0;  // where inside that payload
};

struct TerrainOctree {
    std::uint32_t nodes = 0;
    std::uint32_t leaves = 0;
    std::uint32_t maxDepth = 0;
    std::vector<TerrainLeaf> leafList;   // walk order
};

// Walks one octree payload. `references` maps a node's +16 slot offset to its resolved target, exactly as the unit's
// relocations give it (see DrmFile::relocations / relocAddend). Refuses rather than guesses: child count outside 0-8,
// an offset that leaves the payload, children not in ascending order, or a node visited twice.
bool decodeTerrainOctree(std::span<const std::byte> payload,
                         const std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>& references,
                         TerrainOctree* out, std::string* error = nullptr);


// Terrain strip records inside a type-0 payload (GEOMETRY.md §10), matching cdcEngine tr7's
// `TerrainTextureStripInfo` (`tr7/game/terrain.h:46`): a 44-byte header with vertexCount i32 at +0, matIdx i32 at +20 and
// nextTexture u32 at +40, then `vertexCount` little-endian u16 indices into the unit's stride-20 vertex array. A record's
// nextTexture is an absolute byte offset inside the same payload naming another record, so records chain.
//
// Header shape alone admits far too much: scanning every stored general section of unit 8baf540e finds thousands of
// header-shaped starts whose indices address unrelated bytes - their triangles measure ~21,000 world units against the
// shipped run's median longest edge of 146 [V-data]. Two measured gates separate them (docs/formats/GEOMETRY.md §13):
//   * locality: a triangle strip reuses its neighbours, so in a real index run consecutive values sit close together. Of
//     the records whose triangles are geometry, 98% have at least seven in ten consecutive pairs within eight places; of
//     the records that are not geometry, 56% clear that bar **[V-data]**.
//   * edge scale: the record's own median longest triangle edge must stay below `kStripEdgeScaleFactor` times the scale of
//     the vertex array it indexes - the same consecutive-triple median §1 measured on that array, so the threshold comes
//     from the unit rather than a constant. Good records sit at 200-300 units, rejected ones at thousands **[V-data]**.
// Chains expand to triangles with the degenerate restarts dropped (an index repeated next to itself, §9). A payload that
// holds something else yields no records; nothing is guessed.
inline constexpr float kStripLocalityFraction = 0.7f;
inline constexpr float kStripEdgeScaleFactor = 4.0f;

struct TerrainStripChain {
    std::vector<std::uint32_t> indices;                      // triangle list: three per triangle
    std::vector<std::int32_t> materialOfTriangle;            // one matIdx per triangle, same order
    std::uint32_t records = 0;                               // accepted records in this payload
    std::uint32_t chained = 0;                               // whose nextTexture names another record here
};

// `vertices` is the unit's own vertex array, which the indices address. The call succeeds whenever the payload could be
// scanned: `out->records == 0` is the honest answer for a payload that holds something else.
bool decodeTerrainStripChain(std::span<const std::byte> payload, std::span<const MeshVertex> vertices,
                             TerrainStripChain* out, std::string* error = nullptr);

}  // namespace oa::cdc
