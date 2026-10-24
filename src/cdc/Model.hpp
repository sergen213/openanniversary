#pragma once
// Character/skeletal model records: the type-0 payloads a model unit (pc-w\lara.drm) carries, each self-describing
// with the engine's model version tag at offset +0, decoded to renderable geometry (docs/formats/GEOMETRY.md §36).
//
// The tag is not decorative [V-exe]: the object/unit resource ctor (objdump of `tra.exe` 0x5db1d0..0x5dc000, a region
// Ghidra left unanalyzed) fatals with "model version (%X) ... does not match" unless the record's first dword equals
// exactly 0x4c20453 (cmp at 0x5db249). The record is the legacy tr7 `Model` struct (prior art tr7/game/Stream/stream.h),
// NOT a PCModelData: field names and offsets below were recovered from the software skin-and-draw path
// (`FUN_00409880`, vertex transforms `FUN_00409140`/`FUN_00409540`) and re-verified byte-exact on pc-w\lara.drm
// sections 4 (171,108 B) / 6 (92,020 B) [V-exe + V-data].
//
// Skin weights do not exist per vertex: every vertex names exactly one segment (bone) at its +10; positions are stored
// in that segment's space. Bone matrices arrive from the animation system as shader constants (`vmoPalette`); a static
// decode therefore uses a bind pose we construct ourselves - chain of pivot translations, rotations identity
// [INFERENCE: our choice, not shipped data]. The shipped bind orientation (which model axis is head-up, handedness) is
// unproven; this decoder maps model +z to world +y (up), model x/y to world x/z.

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "cdc/Geometry.hpp"   // MeshVertex: the stride-20 record the renderer already consumes

namespace oa::cdc {

// The dword every shipped model record starts with, checked verbatim by the loader at 0x5db249 [V-exe].
inline constexpr std::uint32_t kModelVersionTag = 0x04c20453u;

struct ModelRecord {
    // Header fields proven present in both lara records [V-data]; names from the tr7 struct recovery (GEOMETRY.md §36).
    std::uint32_t tag = 0;             // +0x00: kModelVersionTag [V-exe]
    std::uint32_t numSegments = 0;     // +0x04: 109 / - ; segments below this are real bones, at/above are virtual
    std::uint32_t field08 = 0;         // +0x08: 114 / 93 - the draw loop walks numSegments+this segment records
    std::uint32_t segmentList = 0;     // +0x0c: 416 / 240 - the segment array start (stride 0x40) [V-data]
    float modelScale[3] = {0, 0, 0};   // +0x10..+0x18: quantiser for vertex i16 positions (0.0042099 in both; the
                                       // fourth float at +0x1c is a w of 1.0) [V-exe: diag scale matrix, FUN_00403040]
    std::uint32_t numVertices = 0;     // +0x20: 6679 / 2221 - the vertex array's count (stride 16)
    std::uint32_t vertexList = 0;      // +0x24: 15872 / 13184 - the vertex array start [V-exe: vtx = *(m+0x24)+i*0x10]
    std::uint32_t numNormals = 0;      // +0x28: 1 / 1 - count of the external normal table (relocated into section 28)
    std::uint32_t normalList = 0;      // +0x2c: 7997 / 3082 - stored addend of a kind-0 relocation into section 28,
                                       // the external normal table [V-data reloc]; payload-relative reading unproven
    std::uint32_t numFaces = 0;        // +0x30: s4 reads 7997 here - the strips' face census (7,989 walked + 8)
    std::uint32_t faceList = 0;        // +0x34: 0 (s4, unrelocated) / 48720 (s6) - stored addend of a face table
    float maxRad = 0.0f;               // +0x3c: 509.16 / 509.925 bounding radius [V-exe: compared in draw cull]
    float maxRadSq = 0.0f;             // +0x40: 509.16^2 measured (2.6e5) - the square of the above [V-data]
    std::uint32_t colourList = 0;      // +0x4c: 27016 / 10416 - stored addend of the per-vertex ARGB array (§36: it
                                       // lands at section 28 offset 0x12c via a kind-0 relocation, NOT payload-relative)
    std::uint32_t pairA = 0;           // +0x50/+0x54: (280, 276)/(1512, 1508); second = first-4 in both records. The
                                       // prior-art `boneMirrorData` names do not fit the bytes [V-data]; roles unproven.
    std::uint32_t pairB = 0;
    std::uint32_t stripList = 0;       // +0x58: 122736 / 73376 - head of the linked triangle-list strips [V-exe: rec =
                                       // model+0x58, indices at rec+0x14, next at rec+0x10]
    std::size_t payloadSize = 0;       // size of the span parsed, kept for bounds reports
};

// Parses one model record header from a type-0 payload. Fails (explaining why) on a foreign tag, a segment array that
// does not fit, a vertex array that does not fit, or a strip head outside the payload; both shipped lara records pass.
bool parseModelRecord(std::span<const std::byte> payload, ModelRecord* out, std::string* error);

// Decodes a model record into renderer-ready stride-20 vertices (positions in world units through the bind pose below,
// colours from the per-vertex ARGB array) plus the triangle list gathered by walking every strip. `colours` is the
// section the colourList relocation names (pc-w\lara.drm section 28): pass payload[0x12c .. ] as a span, or empty to
// bind the shipped default grey (0x80808080 [V-exe: null-pointer fallback in FUN_00409880]). The bind pose is OURS:
// bone k sits at parent-chain pivot sums, virtual segments blend their two bones' positions; model +z maps to world +y.
// The strip records come in two shapes, measured on pc-w\lara.drm section 4 [V-data]: the header's signed dword at +8
// is a self-offset to the strip's vertex block - UV pairs (u16 s,t per referenced vertex at 1/4096, the terrain scale)
// followed by the u16 index run into the model's global position array. A zero +8 means an inline untextured strip:
// indices straight at +0x14 with no UV block (the engine binds white for those, as terrain §23 does). The header word
// at +4 packs a page key in its low byte: values 5..17 measured, and the unit's type-5 sections carry ids 2..72 -
// the same id-names-pages namespace as terrain §21, with plain ids (no class prefix).
struct ModelRun {
    std::uint32_t pageId = 0;   // strip +4 low byte; 0 = untextured, bind white
    std::vector<MeshVertex> vertices;   // one per index slot: position copied, texS/texT from the UV block
    std::vector<std::uint32_t> indices; // consecutive into `vertices` (the index run is per-triangle-corner)
};
struct ModelMesh {
     std::vector<MeshVertex> vertices;
     std::vector<std::uint32_t> indices;
    std::vector<ModelRun> runs;   // textured strips grouped by page id; `vertices`/`indices` carry the inline ones
     float height = 0.0f;          // world-unit vertical extent of the decoded body, for placement reports
     float minY = 0.0f, maxY = 0.0f;
     std::uint32_t stripsWalked = 0;
 };

// Strip `next` slots (+0x10) in the shipped lara unit are kind-0 relocation sites: on disk they hold a section-base
// addend, and the engine patches them to absolute runtime pointers at load. Pass the resolved chain from DrmFile's
// relocations as `stripNext` (slot offset -> payload-relative next record, negative = leaves this section = end of
// chain); an empty map falls back to reading raw slots, which is only correct for synthetic payloads that store plain
// offsets. With the shipped relocation table applied, lara's chain walks 21 strips / 23,967 indices [V-data].
bool decodeModelMesh(std::span<const std::byte> payload, std::span<const std::byte> colours,
                     const std::map<std::uint32_t, std::int64_t>& stripNext, ModelMesh* out, std::string* error);

}  // namespace oa::cdc
