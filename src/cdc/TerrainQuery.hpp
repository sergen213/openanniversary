#pragma once
// Standing on shipped geometry: a spatial query over the terrain mesh that GEOMETRY.md §13 decoded (a vertex array plus
// strip-record index lists). Nothing here depends on the archive; the caller hands over what it already decoded.
//
// The structure is a uniform grid in the horizontal plane, one bucket per cell, each triangle filed into every cell its
// XY footprint touches. `surfaceAt` answers with the highest surface covering a point, which is what walking needs.
//
// Deliberately out of scope for now: walls and overhangs. The answer comes from a column, so an overhang reports the same
// height as the floor under it, and a wall gives no horizontal push-back; that needs the capsule sweep Milestone 5 calls
// for **[INFERENCE]**. Cell size is the mesh's own median longest triangle edge (198 world units in unit `8baf540e`,
// GEOMETRY.md §13), so a cell holds about one triangle wide **[V-data]** rather than a guessed constant.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cdc/Geometry.hpp"

namespace oa::cdc {

struct TerrainHit {
    float y = 0.0f;              // surface height at the queried point, world units
    std::uint32_t triangle = 0;  // which triangle of the list answered
    std::int32_t material = -1;  // matching entry of `materials`, or -1 when none was supplied
};

class TerrainQuery {
public:
    // Builds over a vertex array, a triangle list (three indices per triangle) and one material id per triangle - pass an
    // empty span when materials are unknown. Fails only on inputs that cannot describe a mesh: fewer than three vertices,
    // an index list that is not a whole number of triangles, or an index outside the array.
    bool build(std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices,
               std::span<const std::int32_t> materials, std::string* error = nullptr);

    // Highest surface at or below `fromY + above` covering (x, z), where `above` lets a foot resting on the surface still
    // find it. False when no triangle covers that point - a hole in the level, or outside the mesh entirely.
    bool surfaceAt(float x, float z, float fromY, float above, TerrainHit* out) const;

    std::uint32_t triangleCount() const { return static_cast<std::uint32_t>(triangles_.size()); }
    float cellSize() const { return cellSize_; }
    std::uint32_t cellsX() const { return cols_; }
    std::uint32_t cellsZ() const { return rows_; }
    const float* boundsMin() const { return boundsMin_; }
    const float* boundsMax() const { return boundsMax_; }

    // The corners of a triangle the query reported, as three x, y, z triples. Diagnostic: it shows whether a hit stands on
    // a surface next to the point or on a polygon that spans half the level.
    void triangleCorners(std::uint32_t triangle, float out[9]) const;

private:
    struct Triangle {
        float x0, z0, y0;   // first vertex plus edge vectors, so the covering test and height need no array lookups
        float dx1, dz1, dy1;
        float dx2, dz2, dy2;
        std::int32_t material;
    };

    std::vector<Triangle> triangles_;
    std::vector<std::vector<std::uint32_t>> cells_;
    float boundsMin_[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax_[3] = {0.0f, 0.0f, 0.0f};
    float cellSize_ = 0.0f;
    std::uint32_t cols_ = 0;
    std::uint32_t rows_ = 0;
    int cellOfX(float x) const;   // -1 when the coordinate falls outside the grid
    int cellOfZ(float z) const;
    int cellIndex(float value, float origin, std::uint32_t count) const;
};

}  // namespace oa::cdc
