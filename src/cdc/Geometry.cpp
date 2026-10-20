#include "cdc/Geometry.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

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

}  // namespace oa::cdc
