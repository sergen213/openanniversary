#include "cdc/Geometry.hpp"

#include <algorithm>
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

}  // namespace

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
