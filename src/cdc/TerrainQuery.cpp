#include "cdc/TerrainQuery.hpp"

#include <algorithm>
#include <cmath>

namespace oa::cdc {
namespace {
// Filing a triangle must cover every cell its footprint touches, so the far edge of a bounding box is pulled back onto
// the grid instead of being rejected the way an out-of-range query point is.
int clampCell(int index, std::uint32_t count) {
    if (index < 0) return 0;
    if (static_cast<std::uint32_t>(index) >= count) return static_cast<int>(count - 1u);
    return index;
}
}  // namespace

bool TerrainQuery::build(std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices,
                         std::span<const std::int32_t> materials, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (vertices.size() < 3u) return fail("terrain query needs at least three vertices, got " + std::to_string(vertices.size()));
    if (indices.empty()) return fail("terrain query has no triangles");
    if (indices.size() % 3u != 0u) {
        return fail("index list holds " + std::to_string(indices.size()) + " entries, not a whole number of triangles");
    }

    triangles_.clear();
    cells_.clear();
    cols_ = rows_ = 0;
    cellSize_ = 0.0f;

    const auto px = [&](std::uint32_t i) { return static_cast<float>(vertices[i].x); };
    const auto py = [&](std::uint32_t i) { return static_cast<float>(vertices[i].y); };
    const auto pz = [&](std::uint32_t i) { return static_cast<float>(vertices[i].z); };

    boundsMin_[0] = boundsMin_[1] = boundsMin_[2] = 3.0e38f;
    boundsMax_[0] = boundsMax_[1] = boundsMax_[2] = -3.0e38f;
    std::vector<float> longestEdge;
    longestEdge.reserve(indices.size() / 3u);
    for (std::size_t t = 0; t < indices.size(); t += 3u) {
        const std::uint32_t a = indices[t], b = indices[t + 1u], c = indices[t + 2u];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
            return fail("index " + std::to_string(t) + " names vertex outside the array (" + std::to_string(vertices.size())
                        + " vertices)");
        }
        Triangle tri{px(a), pz(a), py(a), px(b) - px(a), pz(b) - pz(a), py(b) - py(a),
                     px(c) - px(a), pz(c) - pz(a), py(c) - py(a),
                     t / 3u < materials.size() ? materials[t / 3u] : -1};
        triangles_.push_back(tri);

        const float edges[3] = {std::hypotf(tri.dx1, std::hypotf(tri.dy1, tri.dz1)),
                                std::hypotf(tri.dx2, std::hypotf(tri.dy2, tri.dz2)),
                                std::hypotf(tri.dx2 - tri.dx1, std::hypotf(tri.dy2 - tri.dy1, tri.dz2 - tri.dz1))};
        longestEdge.push_back(*std::max_element(edges, edges + 3u));

        const float xs[3] = {tri.x0, tri.x0 + tri.dx1, tri.x0 + tri.dx2};
        const float ys[3] = {tri.y0, tri.y0 + tri.dy1, tri.y0 + tri.dy2};
        const float zs[3] = {tri.z0, tri.z0 + tri.dz1, tri.z0 + tri.dz2};
        for (int i = 0; i < 3; ++i) {
            boundsMin_[0] = std::min(boundsMin_[0], xs[i]);
            boundsMin_[1] = std::min(boundsMin_[1], ys[i]);
            boundsMin_[2] = std::min(boundsMin_[2], zs[i]);
            boundsMax_[0] = std::max(boundsMax_[0], xs[i]);
            boundsMax_[1] = std::max(boundsMax_[1], ys[i]);
            boundsMax_[2] = std::max(boundsMax_[2], zs[i]);
        }
    }

    // Cell size is the mesh's own median longest edge: about one triangle per cell across a level, and it needs no tuned
    // constant (unit 8baf540e measures 198 world units there, docs/formats/GEOMETRY.md §13).
    std::size_t middle = longestEdge.size() / 2u;
    std::nth_element(longestEdge.begin(), longestEdge.begin() + static_cast<std::ptrdiff_t>(middle), longestEdge.end());
    cellSize_ = longestEdge[middle];
    if (!(cellSize_ > 0.0f)) cellSize_ = 1.0f;   // a flat degenerate mesh still needs some grid

    const float extentX = boundsMax_[0] - boundsMin_[0];
    const float extentZ = boundsMax_[2] - boundsMin_[2];
    cols_ = static_cast<std::uint32_t>(extentX / cellSize_) + 1u;
    rows_ = static_cast<std::uint32_t>(extentZ / cellSize_) + 1u;
    // ponytail: one million buckets is the ceiling on a level this size (a 55k-wide level at a 200-unit cell is 276x276);
    // enlarge the cell rather than allocate past it. A denser structure (their oct-tree, once its leaf extents are known)
    // replaces this if bucket scans ever show up in a profile.
    while (static_cast<std::uint64_t>(cols_) * rows_ > 1'000'000ull) {
        cellSize_ *= 2.0f;
        cols_ = static_cast<std::uint32_t>(extentX / cellSize_) + 1u;
        rows_ = static_cast<std::uint32_t>(extentZ / cellSize_) + 1u;
    }
    cells_.assign(static_cast<std::size_t>(cols_) * rows_, {});

    for (std::size_t t = 0; t < triangles_.size(); ++t) {
        const Triangle& tri = triangles_[t];
        const float xs[3] = {tri.x0, tri.x0 + tri.dx1, tri.x0 + tri.dx2};
        const float zs[3] = {tri.z0, tri.z0 + tri.dz1, tri.z0 + tri.dz2};
        const int minCol = clampCell(cellOfX(std::min({xs[0], xs[1], xs[2]})), cols_);
        const int maxCol = clampCell(cellOfX(std::max({xs[0], xs[1], xs[2]})), cols_);
        const int minRow = clampCell(cellOfZ(std::min({zs[0], zs[1], zs[2]})), rows_);
        const int maxRow = clampCell(cellOfZ(std::max({zs[0], zs[1], zs[2]})), rows_);
        for (int row = minRow; row <= maxRow; ++row) {
            for (int col = minCol; col <= maxCol; ++col) {
                cells_[static_cast<std::size_t>(row) * cols_ + static_cast<std::size_t>(col)].push_back(
                    static_cast<std::uint32_t>(t));
            }
        }
    }

    if (error != nullptr) error->clear();
    return true;
}

bool TerrainQuery::surfaceAt(float x, float z, float fromY, float above, TerrainHit* out) const {
    if (out == nullptr || cells_.empty()) return false;
    const int col = cellOfX(x);
    const int row = cellOfZ(z);
    if (col < 0 || row < 0 || static_cast<std::uint32_t>(col) >= cols_ || static_cast<std::uint32_t>(row) >= rows_) {
        return false;   // outside the mesh entirely
    }

    bool found = false;
    TerrainHit best{};
    for (const std::uint32_t index : cells_[static_cast<std::size_t>(row) * cols_ + static_cast<std::size_t>(col)]) {
        const Triangle& tri = triangles_[index];
        // Signed area of the XY projection: a near-zero value is a wall, which this model does not answer for (see header).
        const float cross = tri.dx1 * tri.dz2 - tri.dz1 * tri.dx2;
        if (std::fabs(cross) < 1.0e-3f) continue;
        const float dx = x - tri.x0;
        const float dz = z - tri.z0;
        const float u = (dx * tri.dz2 - dz * tri.dx2) / cross;
        const float v = (tri.dx1 * dz - tri.dz1 * dx) / cross;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;   // the point sits outside this triangle in plan
        const float y = tri.y0 + u * tri.dy1 + v * tri.dy2;
        if (y > fromY + above) continue;                     // a ceiling, not the surface under foot
        if (!found || y > best.y) {
            best.y = y;
            best.triangle = index;
            best.material = tri.material;
            found = true;
        }
    }
    if (found) *out = best;
    return found;
}

int TerrainQuery::cellOfX(float x) const { return cellIndex(x, boundsMin_[0], cols_); }
int TerrainQuery::cellOfZ(float z) const { return cellIndex(z, boundsMin_[2], rows_); }

int TerrainQuery::cellIndex(float value, float origin, std::uint32_t count) const {
    if (cellSize_ <= 0.0f) return -1;
    const long long index = static_cast<long long>((value - origin) / cellSize_);
    if (index < 0 || static_cast<std::uint32_t>(index) >= count) return -1;
    return static_cast<int>(index);
}

void TerrainQuery::triangleCorners(std::uint32_t triangle, float out[9]) const {
    if (triangle >= triangles_.size()) {   // the caller asked about a triangle this query does not hold
        for (int i = 0; i < 9; ++i) out[i] = 0.0f;
        return;
    }
    const Triangle& tri = triangles_[triangle];
    out[0] = tri.x0; out[1] = tri.y0; out[2] = tri.z0;
    out[3] = tri.x0 + tri.dx1; out[4] = tri.y0 + tri.dy1; out[5] = tri.z0 + tri.dz1;
    out[6] = tri.x0 + tri.dx2; out[7] = tri.y0 + tri.dy2; out[8] = tri.z0 + tri.dz2;
}

}  // namespace oa::cdc
