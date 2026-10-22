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

#include "cdc/Drm.hpp"

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
// `vertexCount` little-endian u16 indices into the unit's stride-20 vertex array. nextTexture is a byte offset inside the
// *same* payload naming the next record - its relocation entry names this very section (see `relocSectionIndex`) - and the
// walk continues while the header reached has a non-zero vertexCount, which is prior art's own loop condition
// (`tr7/game/pc/D3D/d3dterrain.cpp:117`). In unit `8baf540e` chains lie back to back inside one payload and each chain's last
// link names a zero-count marker header; the following chain starts four bytes later, at the offset that marker would reach
// if its own count were non-zero (§16).
//
// Chains are entered only from an oct-tree leaf reference (`decodeTerrainOctree`), never by scanning for header-shaped byte
// patterns: a scan over unit `8baf540e` admits thousands of starts sitting inside another record's index run, and those - not
// the shipped data - are what §13's census and §14's long joins were mixed with [V-data]. What a chain walk keeps is
// structural: `vertexCount` in 3..8192 and a multiple of three (measured for every record the leaves reach), the run inside
// this payload, every index naming a vertex this array holds, and each record emitted once however many leaves reach it.
struct TerrainStripChain {
    std::vector<std::uint32_t> indices;             // triangle list, absolute vertex numbers when a base table is given
    std::vector<std::int32_t> materialOfTriangle;   // one matIdx per triangle, same order
    std::uint32_t unknownMaterials = 0;   // records naming a material the base table has no entry for
    std::uint32_t heads = 0;        // leaf references that produced at least one record
    std::uint32_t records = 0;      // distinct records walked across all chains
    std::uint32_t badHeads = 0;     // leaf references whose bytes are not a record
    std::uint32_t brokenLinks = 0;  // nextTexture values that do not name another record here
};

// `heads` are the record offsets an oct-tree's leaves resolve to inside this payload, `vertexCount` is the size of the
// vertex array those indices address. The call succeeds whenever the payload could be walked: `records == 0` with
// `badHeads == heads.size()` is the honest answer for a payload holding something else.
//
// `materialBase`, when given, maps a record's matIdx to the vertex base its indices are relative to (§20): every index
// of such a record must fit inside that material's block, and the numbers emitted are absolute (`base + index`). A
// record naming a material with no entry is not drawn; it lands in `unknownMaterials`.
bool decodeTerrainStripChains(std::span<const std::byte> payload, std::span<const std::uint32_t> heads,
                              std::size_t vertexCount, TerrainStripChain* out, std::string* error = nullptr,
                              std::span<const std::uint32_t> materialBase = {});

// The per-material vertex base offsets a level unit's strip records lean on (GEOMETRY.md §20). Prior art keeps the same
// value in `XboxPcMaterialStripList.vbBaseOffset` and hands it to `DrawIndexedPrimitive` as the start vertex
// (`tr7/game/pc/D3D/d3dterrain.cpp:244`, `tr7/game/terrain.h:67`). In Anniversary's units the list is stored in a type-0
// section of 20-byte records, one per material **in file order**, whose +8 slot is patched by a kind-0 relocation naming
// this very section. Measured on unit `4956BEA0` (Croft Manor entrance hall): its section 43 holds exactly 137 such
// records, the strip chains of that unit name material indices 0..136 with no gaps, and each chain's largest index is at
// or just under the capacity implied by the next base - 16,096/16,097, 16,349/16,350, 16,092/16,093 and 8,077/8,078 for
// four of the five blocks - so all 2,339 records fit without a single overflow [V-data].
struct TerrainMaterialList {
    std::vector<std::uint32_t> vertexBase;   // one base per material, file order = matIdx
    // The raw word at record +12, kept as the (class << 16 | value) pair §21 measured. Class 32's value names the
    // unit section index holding that material's texture page (160 of 172 records across both probe units); other
    // classes name ids in a second namespace that is neither the directory nor the object list. Nothing here decides
    // what a class means - the caller checks before it looks anything up.
    std::vector<std::uint32_t> classValue;
};

// Reads that table from one section payload. Refuses rather than guesses: fewer than eight relocation slots at a stride
// of 20 naming this same section (a unit without such a table), a word at +4 of a record that is not zero, or bases that
// decrease along the run. `selfSection` is the index of `payload`'s section in the unit.
bool decodeTerrainMaterialBases(std::span<const std::byte> payload, const std::vector<DrmReloc>& relocs,
                                std::uint32_t selfSection, TerrainMaterialList* out, std::string* error = nullptr);

// Finds a level unit's base table among its sections and returns it. §20 left first-match selection open; sweep over all 50
// oct-tree units (openanniversary-work/sweep-bases3) shows the rule that picks exactly one run everywhere is **census**: walk
// every tree without bases once, read each record's matIdx at header +20 (§10), and accept a decoded table only when its
// length covers all named indices (max < size). 45 units have runs ≥8; the census rule yields exactly one covering run in 35,
// zero in 5 (no tables); among multi-run units every other fitting candidate leaves unknownMaterials > 0 [V-data]. This call
// does that: it walks chains itself so callers do not duplicate their own tree walk. `out` is empty and false when the unit
// has no oct-tree or no table covers its census; on true, *sectionOut names where the run lives for logging.
bool findTerrainMaterialBases(const DrmFile& drm, std::uint32_t vertexArraySection, TerrainMaterialList* out,
                              std::size_t* sectionOut = nullptr, std::string* error = nullptr);

}  // namespace oa::cdc
