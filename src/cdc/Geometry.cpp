#include "cdc/Geometry.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace oa::cdc {

bool decodeMesh(std::span<const std::byte> payload, std::vector<MeshVertex>* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeMesh needs an output vector");
    if (payload.size() < kMeshVertexBytes * 3u) {
        return fail("geometry payload is " + std::to_string(payload.size()) + " bytes, too short for a triangle ("
                    "needs at least " + std::to_string(kMeshVertexBytes * 3u) + ")");
    }
    if (payload.size() % kMeshVertexBytes != 0u) {
        return fail("geometry payload is " + std::to_string(payload.size()) + " bytes, not a whole number of "
                    + std::to_string(kMeshVertexBytes) + "-byte vertices");
    }

    // Records are read at arbitrary byte offsets inside the archive, so they are copied out one at a time rather
    // than reinterpreted in place: an odd payload offset would make every field access misaligned.
    const std::size_t count = payload.size() / kMeshVertexBytes;
    std::vector<MeshVertex> vertices(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::memcpy(vertices.data() + index, payload.data() + index * kMeshVertexBytes, kMeshVertexBytes);
    }
    out->swap(vertices);
    if (error != nullptr) error->clear();
    return true;
}

MeshBounds meshBounds(const std::vector<MeshVertex>& vertices) {
    MeshBounds bounds{};
    for (int axis = 0; axis < 3; ++axis) {
        bounds.min[axis] = std::numeric_limits<float>::max();
        bounds.max[axis] = std::numeric_limits<float>::lowest();
    }
    for (const MeshVertex& vertex : vertices) {
        const float position[3] = {static_cast<float>(vertex.x), static_cast<float>(vertex.y),
                                   static_cast<float>(vertex.z)};
        for (int axis = 0; axis < 3; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], position[axis]);
        }
    }
    if (vertices.empty()) {
        for (int axis = 0; axis < 3; ++axis) {
            bounds.min[axis] = 0.0f;
            bounds.max[axis] = 0.0f;
        }
    }
    bounds.triangles = static_cast<std::uint32_t>(vertices.size() / 3u);
    return bounds;
}

namespace {

// Payloads sit at arbitrary byte offsets inside an archive record, so every field is copied out rather than
// reinterpreted in place.
std::uint32_t loadU32(const std::byte* at) {
    std::uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

// Index arrays are read the same way: copied out, because payloads sit at arbitrary byte offsets.
std::uint16_t loadU16(const std::byte* at) {
    std::uint16_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
}  // namespace



bool decodeTerrainStripChains(std::span<const std::byte> payload, std::span<const std::uint32_t> heads,
                               std::size_t vertexCount, TerrainStripChain* out, std::string* error) {
    constexpr std::size_t kHeaderBytes = 44;   // GEOMETRY.md §10, prior art tr7/game/terrain.h:46
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainStripChains needs an output struct");
    out->indices.clear();
    out->materialOfTriangle.clear();
    out->heads = out->records = out->badHeads = out->brokenLinks = out->snappedLinks = 0u;

    // What makes a record here is structure, not statistics: vertexCount a multiple of three within a sane range, the
    // index run inside this payload, and every index naming a vertex of the array (GEOMETRY.md §15 measured all 4,768
    // chain-walked records against those tests, with no 0xFFFF restart word among them).
    const auto readRecord = [&](std::size_t at, std::uint32_t* count, std::int32_t* matIdx, std::uint32_t* next) {
        if (at + kHeaderBytes > payload.size()) return false;
        const std::byte* base = payload.data() + at;
        const std::uint32_t vertexCountHere = loadU32(base);
        if (vertexCountHere < 3u || vertexCountHere > 8192u || vertexCountHere % 3u != 0u) return false;
        if (at + kHeaderBytes + 2u * static_cast<std::size_t>(vertexCountHere) > payload.size()) return false;
        for (std::uint32_t i = 0; i < vertexCountHere; ++i) {
            if (static_cast<std::size_t>(loadU16(base + kHeaderBytes + 2u * i)) >= vertexCount) return false;
        }
        *count = vertexCountHere;
        *matIdx = static_cast<std::int32_t>(loadU32(base + 20u));
        *next = loadU32(base + 40u);
        return true;
    };

    std::vector<std::size_t> walked;   // record offsets already emitted, so a shared tail is drawn once
    for (std::uint32_t head : heads) {
        std::size_t at = head;
        bool producedAny = false;
        while (true) {
            std::uint32_t count = 0u;
            std::uint32_t next = 0u;
            std::int32_t matIdx = 0;
            if (!readRecord(at, &count, &matIdx, &next)) {
                if (producedAny) out->brokenLinks += 1u; else out->badHeads += 1u;
                break;
            }
            if (std::find(walked.begin(), walked.end(), at) != walked.end()) {   // shared tail or a cycle
                if (!producedAny) out->heads += 1u;   // this leaf's bytes are a record, drawn by another chain
                break;
            }
            walked.push_back(at);
            producedAny = true;
            out->records += 1u;
            const std::byte* run = payload.data() + at + kHeaderBytes;
            for (std::uint32_t i = 0; i + 2u < count; i += 3u) {
                out->indices.push_back(loadU16(run + 2u * i));
                out->indices.push_back(loadU16(run + 2u * (i + 1u)));
                out->indices.push_back(loadU16(run + 2u * (i + 2u)));
                out->materialOfTriangle.push_back(matIdx);
            }
            // Chains end on a null nextTexture (prior art tr7/game/terrain.h:46); the shipped records that end a chain
            // measured here hold 0xFFFFFFFF in that slot instead of zero, so both terminate [V-data].
            if (next == 0u || next == 0xFFFFFFFFu) break;
            // A link naming a valid header is used exactly as stored. The file form pads record runs to keep headers on
            // an even byte and patches these slots through relocations, which leaves part of them short of the header they
            // mean: measured over unit `8baf540e`'s strip payloads, 139 of 750 links name a spot exactly four bytes before
            // the record that follows it (docs/formats/GEOMETRY.md §16). Stepping forward to a real header is allowed - up
            // to two words, and only when the bytes there pass every record test. Anything else ends the chain.
            bool linked = false;
            for (std::size_t extra = 0u; extra <= 8u; extra += 4u) {
                const std::size_t candidate = static_cast<std::size_t>(next) + extra;
                std::uint32_t linkCount = 0u, linkNext = 0u;
                std::int32_t linkMat = 0;
                if (!readRecord(candidate, &linkCount, &linkMat, &linkNext)) continue;
                if (extra != 0u) out->snappedLinks += 1u;
                at = candidate;
                linked = true;
                break;
            }
            if (!linked) {
                out->brokenLinks += 1u;
                break;
            }
        }
        if (producedAny) out->heads += 1u;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool decodeTerrainOctree(std::span<const std::byte> payload,
                         const std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>& references,
                         TerrainOctree* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainOctree needs an output struct");
    if (payload.size() < 24u) return fail("oct-tree payload is " + std::to_string(payload.size()) + " bytes, too small "
                                          "for one node (needs at least 24)");

    TerrainOctree tree;
    std::vector<std::pair<std::uint32_t, unsigned>> stack{{0u, 0u}};   // (node offset, depth)
    std::vector<bool> visited(payload.size() / 4u + 1u, false);
    while (!stack.empty()) {
        const auto [offset, depth] = stack.back();
        stack.pop_back();
        if (offset % 4u != 0u || offset / 4u >= visited.size() || visited[offset / 4u]) {
            return fail("oct-tree node at " + std::to_string(offset) + " was already visited or misaligned");
        }
        visited[offset / 4u] = true;
        if (offset + 24u > payload.size()) {
            return fail("oct-tree node at " + std::to_string(offset) + " runs past the end of the payload");
        }
        const auto childCount = static_cast<std::int32_t>(loadU32(payload.data() + offset + 20u));
        if (childCount < 0 || childCount > 8) {
            return fail("oct-tree node at " + std::to_string(offset) + " claims " + std::to_string(childCount)
                        + " children, outside the measured range 0-8");
        }
        tree.nodes += 1;
        tree.maxDepth = depth > tree.maxDepth ? depth : tree.maxDepth;
        if (childCount == 0) {
            const auto reference = references.find(offset + 16u);
            if (reference == references.end()) {
                return fail("oct-tree leaf at " + std::to_string(offset) + " has no relocation reference in its +16 slot");
            }
            tree.leaves += 1;
            tree.leafList.push_back({offset, reference->second.first, reference->second.second});
            continue;
        }
        std::uint32_t previous = offset;
        for (std::int32_t child = 0; child < childCount; ++child) {
            const std::uint32_t at = offset + 24u + 4u * static_cast<std::uint32_t>(child);
            const std::uint32_t target = loadU32(payload.data() + at);
            if (target <= previous || target + 24u > payload.size()) {
                return fail("oct-tree node at " + std::to_string(offset) + " has a child offset " 
                            + std::to_string(target) + " outside the payload or out of order");
            }
            previous = target;
            stack.emplace_back(target, depth + 1);
        }
    }
    *out = std::move(tree);
    if (error != nullptr) error->clear();
    return true;
}

}  // namespace oa::cdc
