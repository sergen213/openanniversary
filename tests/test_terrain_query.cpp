#include "cdc/TerrainQuery.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using oa::cdc::MeshVertex;
using oa::cdc::TerrainHit;
using oa::cdc::TerrainQuery;

MeshVertex vertex(std::int16_t x, std::int16_t y, std::int16_t z) {
    MeshVertex out;
    out.x = x;
    out.y = y;
    out.z = z;
    return out;
}

// A 100x100 square sloping at half a unit of height per unit of x: y = x / 2. Two triangles, materials 7 and 9.
std::vector<MeshVertex> slope() {
    return {vertex(0, 0, 0), vertex(100, 50, 0), vertex(100, 50, 100), vertex(0, 0, 100)};
}
const std::vector<std::uint32_t> kSquare{{0u, 1u, 2u, 0u, 2u, 3u}};

TEST(TerrainQuery, standsOnTheHighestSurfaceUnderFoot) {
    TerrainQuery terrain;
    const std::vector<std::int32_t> materials{7, 9};
    std::string error;
    ASSERT_TRUE(terrain.build(slope(), kSquare, materials, &error)) << error;

    TerrainHit hit;
    // Dead centre: x = 50 gives y = 25 on this plane.
    ASSERT_TRUE(terrain.surfaceAt(50.0f, 50.0f, 100.0f, 1.0f, &hit));
    EXPECT_NEAR(hit.y, 25.0f, 1.0e-3f);
    EXPECT_TRUE(hit.material == 7 || hit.material == 9);
    // The triangle that answered is one of the two, and its own corner heights bracket the height it reported.
    float corners[9];
    terrain.triangleCorners(hit.triangle, corners);
    EXPECT_GE(std::max({corners[1], corners[4], corners[7]}), hit.y - 1.0e-3f);
    EXPECT_LE(std::min({corners[1], corners[4], corners[7]}), hit.y + 1.0e-3f);


    // A foot resting just above the surface still finds it; one below it does not (that is a ceiling).
    ASSERT_TRUE(terrain.surfaceAt(20.0f, 60.0f, 10.5f, 0.6f, &hit));
    EXPECT_NEAR(hit.y, 10.0f, 1.0e-3f);
    EXPECT_FALSE(terrain.surfaceAt(20.0f, 60.0f, 9.4f, 0.0f, &hit));

    // Off the mesh entirely: no triangle covers the point.
    EXPECT_FALSE(terrain.surfaceAt(-5.0f, 50.0f, 100.0f, 1.0f, &hit));
    EXPECT_FALSE(terrain.surfaceAt(50.0f, 400.0f, 100.0f, 1.0f, &hit));
}

TEST(TerrainQuery, picksTheTopFloorOfTwoStackedPlanes) {
    // The same XY footprint twice: an upper floor at y = 0 and a pit bottom at y = -30.
    std::vector<MeshVertex> vertices{vertex(0, 0, 0), vertex(100, 0, 0), vertex(100, 0, 100), vertex(0, 0, 100),
                                     vertex(0, -30, 0), vertex(100, -30, 0), vertex(100, -30, 100), vertex(0, -30, 100)};
    const std::vector<std::uint32_t> indices{0u, 1u, 2u, 0u, 2u, 3u, 4u, 5u, 6u, 4u, 6u, 7u};
    TerrainQuery terrain;
    std::string error;
    ASSERT_TRUE(terrain.build(vertices, indices, {}, &error)) << error;

    TerrainHit hit;
    ASSERT_TRUE(terrain.surfaceAt(50.0f, 50.0f, 5.0f, 1.0f, &hit));
    EXPECT_NEAR(hit.y, 0.0f, 1.0e-3f);
    EXPECT_EQ(hit.material, -1);   // no materials were supplied

    ASSERT_TRUE(terrain.surfaceAt(50.0f, 50.0f, -10.0f, 0.0f, &hit));
    EXPECT_NEAR(hit.y, -30.0f, 1.0e-3f);
}

TEST(TerrainQuery, aWallInThePlanGivesNoSupport) {
    // One vertical triangle: projected on the plan it is a line, so it has no area to stand on (see TerrainQuery.hpp).
    std::vector<MeshVertex> vertices{vertex(0, 0, 0), vertex(0, 50, 100), vertex(0, 0, 200)};
    const std::vector<std::uint32_t> indices{0u, 1u, 2u};
    TerrainQuery terrain;
    std::string error;
    ASSERT_TRUE(terrain.build(vertices, indices, {}, &error)) << error;
    EXPECT_EQ(terrain.triangleCount(), 1u);

    TerrainHit hit;
    EXPECT_FALSE(terrain.surfaceAt(0.0f, 50.0f, 100.0f, 1.0f, &hit));
}

TEST(TerrainQuery, refusesInputsThatCannotBeAMesh) {
    std::string error;
    TerrainQuery terrain;

    EXPECT_FALSE(terrain.build({}, kSquare, {}, &error));
    EXPECT_NE(error.find("three vertices"), std::string::npos) << error;

    EXPECT_FALSE(terrain.build(slope(), {}, {}, &error));
    EXPECT_NE(error.find("no triangles"), std::string::npos) << error;

    const std::vector<std::uint32_t> ragged{0u, 1u, 2u, 3u};
    EXPECT_FALSE(terrain.build(slope(), ragged, {}, &error));
    EXPECT_NE(error.find("whole number of triangles"), std::string::npos) << error;

    const std::vector<std::uint32_t> outOfRange{0u, 1u, 99u};
    EXPECT_FALSE(terrain.build(slope(), outOfRange, {}, &error));
    EXPECT_NE(error.find("outside the array"), std::string::npos) << error;

    // Nothing survives a refusal: an empty query answers nothing rather than guessing.
    TerrainHit hit;
    EXPECT_FALSE(terrain.surfaceAt(50.0f, 50.0f, 100.0f, 10.0f, &hit));
}

TEST(TerrainQuery, cellSizeComesOutOfTheMeshItself) {
    TerrainQuery terrain;
    std::string error;
    ASSERT_TRUE(terrain.build(slope(), kSquare, {}, &error)) << error;
    // Both triangles share the same longest edge - across the slope (dx 100, dy 50, dz 100) - so that is the median the
    // grid takes its cell size from.
    EXPECT_NEAR(terrain.cellSize(), 150.0f, 0.01f);
    EXPECT_GE(terrain.cellsX(), 1u);
    EXPECT_GE(terrain.cellsZ(), 1u);
}

}  // namespace
