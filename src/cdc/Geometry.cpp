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



bool decodeTerrainStripChain(std::span<const std::byte> payload, std::span<const MeshVertex> vertices,
                             TerrainStripChain* out, std::string* error) {
    constexpr std::size_t kHeaderBytes = 44;   // GEOMETRY.md §10, prior art tr7/game/terrain.h:46
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainStripChain needs an output struct");
    out->indices.clear();
    out->materialOfTriangle.clear();
    out->records = 0;
    out->chained = 0;
    if (vertices.size() < 3u) return true;   // no array for the indices to address

    const auto position = [&](std::uint32_t i) {
        return std::array<float, 3>{static_cast<float>(vertices[i].x), static_cast<float>(vertices[i].y),
                                    static_cast<float>(vertices[i].z)};
    };
    const auto squaredDistance = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        float sum = 0.0f;
        for (int axis = 0; axis < 3; ++axis) sum += (a[axis] - b[axis]) * (a[axis] - b[axis]);
        return sum;
    };
    const auto medianOf = [](std::vector<float>& values) {
        std::size_t middle = values.size() / 2u;
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
        return values[middle];
    };

    // The scale of the unit's own surface: the median longest edge over consecutive triples, which is how §6 reads a run.
    // Sampled, because a level array holds ~150k vertices and the median does not need all of them.
    std::vector<float> scale;
    for (std::size_t first = 0; first + 3u <= vertices.size(); first += 3u) {
        const auto a = position(static_cast<std::uint32_t>(first));
        const auto b = position(static_cast<std::uint32_t>(first + 1u));
        const auto c = position(static_cast<std::uint32_t>(first + 2u));
        scale.push_back(std::max({squaredDistance(a, b), squaredDistance(b, c), squaredDistance(c, a)}));
        if (scale.size() >= 4096u) break;   // [V-data] unit 8baf540e: median longest edge 146 world units either way
    }
    const float edgeLimit = kStripEdgeScaleFactor * kStripEdgeScaleFactor * medianOf(scale);

    struct Record {
        std::uint32_t offset = 0;
        std::int32_t matIdx = 0;
        std::uint32_t nextTexture = 0;
        std::vector<std::uint32_t> run;   // the index words as stored, restarts included
    };
    const auto u16At = [&](std::size_t at) { return static_cast<std::uint32_t>(loadU16(payload.data() + at)); };

    // Pass one: find record starts. Accepting a record claims the bytes it spans, so two records never overlap and an
    // index run is not mistaken for another header. 0xFFFF ends a strip inside a run like the restarts §9 measured, so
    // those words drop out of both gates as well as out of the triangles.
    std::vector<Record> records;
    for (std::size_t off = 0; off + kHeaderBytes <= payload.size(); off += 2u) {
        const std::uint32_t vertexCount = loadU32(payload.data() + off);
        if (vertexCount < 3u || vertexCount > 8192u) continue;
        const std::size_t indexStart = off + kHeaderBytes;
        const std::size_t end = indexStart + 2u * vertexCount;
        if (end > payload.size()) continue;
        std::vector<std::uint32_t> run;
        run.reserve(vertexCount);
        bool inRange = true;
        for (std::uint32_t i = 0; i < vertexCount && inRange; ++i) {
            const std::uint32_t index = u16At(indexStart + 2u * i);
            if (index != 0xFFFFu && index >= vertices.size()) inRange = false;
            run.push_back(index);
        }
        if (!inRange || run.size() < 3u) continue;

        std::size_t close = 0;   // locality: consecutive indices inside a real strip sit near each other
        for (std::size_t i = 1; i < run.size(); ++i) {
            if (run[i] == 0xFFFFu || run[i - 1u] == 0xFFFFu) continue;   // a pair across two strips says nothing
            const std::int64_t delta = static_cast<std::int64_t>(run[i]) - static_cast<std::int64_t>(run[i - 1u]);
            if (delta >= -8 && delta <= 8) close += 1;
        }
        if (static_cast<float>(close) < kStripLocalityFraction * static_cast<float>(run.size() - 1u)) continue;

        std::vector<float> edges;   // edge scale: the record must draw triangles of this unit's own size
        for (std::size_t i = 2; i < run.size(); ++i) {
            const std::uint32_t a = run[i - 2u], b = run[i - 1u], c = run[i];
            if (a == 0xFFFFu || b == 0xFFFFu || c == 0xFFFFu) continue;   // straddles a restart
            edges.push_back(std::max({squaredDistance(position(a), position(b)), squaredDistance(position(b), position(c)),
                                      squaredDistance(position(c), position(a))}));
        }
        if (edges.empty() || medianOf(edges) > edgeLimit) continue;

        records.push_back({static_cast<std::uint32_t>(off), static_cast<std::int32_t>(loadU32(payload.data() + off + 20u)),
                           loadU32(payload.data() + off + 40u), std::move(run)});
        off = end - 2u;   // the loop step lands on `end`, the byte after this record
    }

    const auto startOf = [&](std::uint32_t offset) -> std::size_t {
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (records[i].offset == offset) return i;
        }
        return records.size();
    };

    // Pass two: walk every chain once, so a record reached through nextTexture is not drawn again from its own start.
    TerrainStripChain strips;
    std::vector<char> seen(records.size(), 0);
    for (std::size_t first = 0; first < records.size(); ++first) {
        if (seen[first]) continue;
        for (std::size_t at = first; at < records.size();) {
            if (seen[at]) break;   // a cycle in nextTexture, or this record joins one already walked
            seen[at] = 1;
            const Record& record = records[at];
            strips.records += 1;
            for (std::size_t i = 2; i < record.run.size(); ++i) {
                const std::uint32_t a = record.run[i - 2u], b = record.run[i - 1u], c = record.run[i];
                if (a == 0xFFFFu || b == 0xFFFFu || c == 0xFFFFu) continue;   // straddles a strip restart, §9
                if (a == b || b == c || a == c) continue;                     // degenerate restart, §9
                strips.indices.insert(strips.indices.end(), {a, b, c});
                strips.materialOfTriangle.push_back(record.matIdx);
            }
            const std::size_t next = startOf(record.nextTexture);
            if (next >= records.size()) break;   // the chain ends in this payload
            strips.chained += 1;
            at = next;
        }
    }
    *out = std::move(strips);
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
