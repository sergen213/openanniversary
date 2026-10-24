#include "cdc/Model.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace oa::cdc {

namespace {

// Payloads sit at arbitrary byte offsets inside an archive record, so every field is copied out rather than
// reinterpreted in place (same convention as Geometry.cpp).
std::uint32_t loadU32(const std::byte* at) {
    std::uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::int16_t loadI16(const std::byte* at) {
    std::int16_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::uint16_t loadU16(const std::byte* at) {
    std::uint16_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

float loadFloat(const std::byte* at) {
    float value = 0.0f;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

}  // namespace

bool parseModelRecord(std::span<const std::byte> payload, ModelRecord* out, std::string* error) {
    constexpr std::size_t kHeaderBytes = 0x5c;   // the proven field extent: through +0x58 [V-data]
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("no model record given");
    if (payload.size() < kHeaderBytes) return fail("payload shorter than the 0x5c-byte model header");
    const std::byte* base = payload.data();
    ModelRecord record;
    record.payloadSize = payload.size();
    record.tag = loadU32(base + 0x00);
    if (record.tag != kModelVersionTag) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "model tag is 0x%08x, expected 0x%08x", record.tag, kModelVersionTag);
        return fail(buffer);
    }
    record.numSegments = loadU32(base + 0x04);
    record.field08 = loadU32(base + 0x08);
    record.segmentList = loadU32(base + 0x0c);
    for (int i = 0; i < 3; ++i) record.modelScale[i] = loadFloat(base + 0x10 + 4 * static_cast<std::size_t>(i));
    record.numVertices = loadU32(base + 0x20);
    record.vertexList = loadU32(base + 0x24);
    record.numNormals = loadU32(base + 0x28);
    record.normalList = loadU32(base + 0x2c);
    record.numFaces = loadU32(base + 0x30);
    record.faceList = loadU32(base + 0x34);
    record.maxRad = loadFloat(base + 0x3c);
    record.maxRadSq = loadFloat(base + 0x40);
    record.colourList = loadU32(base + 0x4c);
    record.pairA = loadU32(base + 0x50);
    record.pairB = loadU32(base + 0x54);
    record.stripList = loadU32(base + 0x58);

    // Bounds every shipped record satisfies [V-data: lara sections 4 and 6 do]. The draw loop walks
    // numSegments+field08 segment records [V-exe: "count = model+4 plus model+8" in FUN_00409880]; the array capacity is
    // what the payload can hold, and decode uses the lesser.
    if (record.numSegments == 0u || record.segmentList >= payload.size())
        return fail("segment array start outside the payload");
    const std::size_t segmentCapacity = (payload.size() - record.segmentList) / 0x40u;
    if (static_cast<std::size_t>(record.numSegments) > segmentCapacity)
        return fail("segment count does not fit between the segment array and the payload end");
    const std::size_t vertexBytes = static_cast<std::size_t>(record.numVertices) * 16u;   // stride 16 [V-exe: i*0x10]
    if (static_cast<std::size_t>(record.vertexList) + vertexBytes > payload.size())
        return fail("vertex array does not fit inside the payload");
    if (record.stripList == 0u || record.stripList >= payload.size()) return fail("strip list head outside the payload");
    // No veto on a contiguous index stream: indices live in the linked strips, and lara's colour/normal relocations land
    // past this payload into section 28 (GEOMETRY.md §36), so any fit test beyond the arrays above rejects shipped data.
    *out = record;
    if (error != nullptr) error->clear();
    return true;
}

bool decodeModelMesh(std::span<const std::byte> payload, std::span<const std::byte> colours,
                     const std::map<std::uint32_t, std::int64_t>& stripNext, ModelMesh* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    ModelRecord record;
    if (!parseModelRecord(payload, &record, error)) return false;
    if (out == nullptr) return fail("no model mesh given");
    const std::byte* base = payload.data();

    // Segment positions through the bind pose. Real segments chain parent pivots [V-data: parent i32 at +0x38, pivot
    // float3 at +0x20]; virtual ones blend their two bones' positions and add w * own pivot (the shipped matrix blend is
    // proven; applying it to translations with identity rotations is our bind-pose choice [INFERENCE]).
    const std::size_t segmentCapacity = (payload.size() - record.segmentList) / 0x40u;
    const std::size_t walked = static_cast<std::size_t>(record.numSegments) + record.field08;
    const std::size_t segments = walked <= segmentCapacity ? walked : segmentCapacity;
    std::vector<Vec3> segPos(segments);
    std::vector<char> resolved(segments, 0);
    const auto place = [&](std::size_t k, auto& self) -> bool {
        if (k >= segments) return false;
        if (resolved[k] == 1) return true;
        if (resolved[k] == 2) return false;   // cycle: the shipped loader fatals long before us [INFERENCE]
        resolved[k] = 2;
        const std::byte* rec = base + record.segmentList + k * 0x40u;
        const Vec3 pivot{loadFloat(rec + 0x20), loadFloat(rec + 0x24), loadFloat(rec + 0x28)};
        Vec3 at{};
        if (k < record.numSegments) {
            const std::int32_t parent = static_cast<std::int32_t>(loadU32(rec + 0x38));
            if (parent >= 0 && !self(static_cast<std::size_t>(parent), self)) return false;
            at.x = pivot.x + (parent >= 0 ? segPos[static_cast<std::size_t>(parent)].x : 0.0f);
            at.y = pivot.y + (parent >= 0 ? segPos[static_cast<std::size_t>(parent)].y : 0.0f);
            at.z = pivot.z + (parent >= 0 ? segPos[static_cast<std::size_t>(parent)].z : 0.0f);
        } else {
            // Virtual: bones A/B are u16s packed into the parent slot; weight float at +0x3c [V-exe, §36].
            const std::uint16_t boneA = loadU16(rec + 0x38);
            const std::uint16_t boneB = loadU16(rec + 0x3a);
            const float w = loadFloat(rec + 0x3c);
            if (!self(boneA, self) || !self(boneB, self)) return false;
            const Vec3& a = segPos[boneA];
            const Vec3& b = segPos[boneB];
            at.x = (1.0f - w) * a.x + w * (b.x + pivot.x);
            at.y = (1.0f - w) * a.y + w * (b.y + pivot.y);
            at.z = (1.0f - w) * a.z + w * (b.z + pivot.z);
        }
        segPos[k] = at;
        resolved[k] = 1;
        return true;
    };

    ModelMesh mesh;
    mesh.vertices.resize(record.numVertices);
    for (std::uint32_t i = 0; i < record.numVertices; ++i) {
        const std::byte* vtx = base + record.vertexList + static_cast<std::size_t>(i) * 16u;
        const auto segIndex = static_cast<std::uint32_t>(static_cast<std::uint16_t>(loadI16(vtx + 10)));
        if (segIndex >= segments || !place(segIndex, place)) {
            char buffer[80];
            std::snprintf(buffer, sizeof(buffer), "vertex %u names segment %u, which the record cannot resolve", i, segIndex);
            return fail(buffer);
        }
        const Vec3 local{static_cast<float>(loadI16(vtx + 0)) * record.modelScale[0],
                         static_cast<float>(loadI16(vtx + 2)) * record.modelScale[1],
                         static_cast<float>(loadI16(vtx + 4)) * record.modelScale[2]};
        // Model space is z-up in the shipped data (the pivot chain runs along +z, §36 census); the terrain world is y-up.
        const Vec3 world{segPos[segIndex].x + local.x, segPos[segIndex].z + local.z, segPos[segIndex].y + local.y};
        const auto clamp16 = [](float v) {
            return static_cast<std::int16_t>(v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v));
        };
        MeshVertex& vertex = mesh.vertices[i];
        vertex.x = clamp16(world.x);
        vertex.y = clamp16(world.y);   // i16 world units: Lara's extent (~500) sits far inside the range [V-data]
        vertex.z = clamp16(world.z);
        vertex.flag = 1u;              // §2's constant word, as the octahedron marker also matches
        std::uint32_t argb = 0x80808080u;   // the shipped null-colour fallback [V-exe, FUN_00409880]
        const std::size_t colourAt = static_cast<std::size_t>(i) * 4u;
        if (colourAt + 4u <= colours.size()) argb = loadU32(colours.data() + colourAt);
        // Stride-20 stores the modulate as r,g,b,a bytes at +8..+11; shipped arrays are ARGB dwords.
        vertex.red = static_cast<std::uint8_t>((argb >> 16u) & 0xffu);
        vertex.green = static_cast<std::uint8_t>((argb >> 8u) & 0xffu);
        vertex.blue = static_cast<std::uint8_t>(argb & 0xffu);
        vertex.alpha = static_cast<std::uint8_t>((argb >> 24u) & 0xffu);
        if (i == 0u || world.y < mesh.minY) mesh.minY = world.y;
        if (i == 0u || world.y > mesh.maxY) mesh.maxY = world.y;
    }

    // Walk the linked strips [V-exe: rec = model+0x58; count i16 at +0 (0 terminates), indices u16 at +0x14, next at
    // +0x10]. Every shipped lara `next` slot is a kind-0 relocation site: on disk it holds the addend of an absolute
    // target in another section's base space, and the engine patches it at load (measured on /tmp/lara_unit.bin: all 20
    // interior slots relocated; resolving them walks 21 strips / 23,967 indices, every index < the 6,679-vertex array).
    // `stripNext` carries those resolved payload-relative targets; a negative value means the chain leaves this section.
    std::map<std::uint32_t, bool> visited;
    std::uint32_t rec = record.stripList;
    while (rec != 0u) {
        if (visited[rec]) return fail("strip list loops");
        visited[rec] = true;
        if (static_cast<std::size_t>(rec) + 0x14u > payload.size()) return fail("strip header runs past the payload end");
        const std::int16_t count = loadI16(base + rec);
        if (count == 0) break;
        if (count < 0 || count % 3 != 0) {
            char buffer[80];
            std::snprintf(buffer, sizeof(buffer), "strip at %u has a non-triangle count %d", rec, static_cast<int>(count));
            return fail(buffer);
        }
        const std::size_t indexBytes = static_cast<std::size_t>(count) * 2u;
        if (static_cast<std::size_t>(rec) + 0x14u + indexBytes > payload.size())
            return fail("strip indices run past the payload end");
        for (std::int16_t k = 0; k < count; ++k) {
            const std::uint32_t index = loadU16(base + rec + 0x14u + static_cast<std::size_t>(k) * 2u);
            if (index >= record.numVertices) {
                char buffer[80];
                std::snprintf(buffer, sizeof(buffer), "strip at %u indexes vertex %u past the array", rec, index);
                return fail(buffer);
            }
            mesh.indices.push_back(index);
        }
        ++mesh.stripsWalked;   // counts strips that contributed triangles; a terminating count-0 record is not one
        const auto fixup = stripNext.find(rec + 0x10u);
        if (fixup != stripNext.end()) {
            // The shipped chain's last patched target lands at the payload tail with no room for a record header -
            // measured on lara section 4, where that advance leaves the span. Treat it as the end of the chain.
            if (fixup->second < 0 || static_cast<std::size_t>(fixup->second) + 0x14u > payload.size()) break;
            rec = static_cast<std::uint32_t>(fixup->second);
        } else {
            rec = loadU32(base + rec + 0x10u);   // unrelocated slot (synthetic payloads, unpatched stores)
        }
    }

    mesh.height = mesh.maxY - mesh.minY;
    *out = std::move(mesh);
    if (error != nullptr) error->clear();
    return true;
}

}  // namespace oa::cdc
