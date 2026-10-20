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
// A leaf's +16 reference is the first strip record of that leaf's chain, exactly as cdcEngine tr7 walks it
// (`DrawOctreeSphere` -> `sphere->strip`, `tr7/game/pc/D3D/d3dterrain.cpp:182`): with the section numbering of
// `relocSectionIndex` all 1,612 leaf references of unit `8baf540e` land on a record header whose index run stays inside
// the target payload [V-data] (docs/formats/GEOMETRY.md §15).
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


// Terrain strip records inside a type-0 payload (GEOMETRY.md §10, §15), matching cdcEngine tr7's
// `TerrainTextureStripInfo` (`tr7/game/terrain.h:46`): a 44-byte header with vertexCount i32 at +0, vmoObjectIndex i32
// at +4 (measured -1 in every record these chains reach [V-data]), matIdx i32 at +20 and nextTexture u32 at +40, then
// `vertexCount` little-endian u16 indices into the unit's stride-20 vertex array. nextTexture is a byte offset inside
// the *same* payload naming the next record - its relocation entry names this very section (see `relocSectionIndex`) -
// and a null or 0xFFFFFFFF value ends the chain (prior art's `for (strip = tsi; strip->vertexCount > 0;` walk stops on
// either; the shipped records that end one hold 0xFFFFFFFF in this slot [V-data]).
//
// Chains are entered only from an oct-tree leaf reference (`decodeTerrainOctree`), never by scanning for header-shaped
// byte patterns: a scan over unit `8baf540e` admits thousands of starts that sit inside another record's index run, and
// those, not the shipped data, are what §13's long triangles and §14's 25,832 dropped slivers came from [V-data]. What a
// chain walk keeps is structural - `vertexCount` in 3..8192 and a multiple of three (measured for all 4,768 records the
// leaves reach), the run inside the payload, every index naming a vertex this array holds (a real run contains no 0xFFFF
// restart word: §9's sentinels were scan artefacts [V-data]) - and each record is emitted once however many leaves reach it.
// A link can name a spot four to eight bytes short of the record it means - the file form pads runs and patches these
// slots by relocation, and §16 measured 139 of 750 links in unit `8baf540e` sitting exactly four bytes early. Such a link
// steps forward onto a header that passes every test above and counts the step; one that names nothing valid ends the chain.
struct TerrainStripChain {
    std::vector<std::uint32_t> indices;             // triangle list: three per triangle, run order kept
    std::vector<std::int32_t> materialOfTriangle;   // one matIdx per triangle, same order
    std::uint32_t heads = 0;        // leaf references that produced at least one record
    std::uint32_t records = 0;      // distinct records walked across all chains
    std::uint32_t badHeads = 0;     // leaf references whose bytes are not a record
    std::uint32_t brokenLinks = 0;  // nextTexture values that do not name another record here
    std::uint32_t snappedLinks = 0;  // links stepped forward by up to two words onto a real record header
};

// `heads` are the record offsets an oct-tree's leaves resolve to inside this payload, `vertexCount` is the size of the
// vertex array those indices address. The call succeeds whenever the payload could be walked: `records == 0` with
// `badHeads == heads.size()` is the honest answer for a payload holding something else.
bool decodeTerrainStripChains(std::span<const std::byte> payload, std::span<const std::uint32_t> heads,
                              std::size_t vertexCount, TerrainStripChain* out, std::string* error = nullptr);

}  // namespace oa::cdc
