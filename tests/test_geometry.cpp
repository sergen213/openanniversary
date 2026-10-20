// Unit tests for the unindexed triangle-run decoder (docs/formats/GEOMETRY.md §2, §6). Fixtures are synthetic: the
// golden record below pins the byte order of every field, and the shape rules come from what a 20-byte stride
// implies for payload sizes measured over the shipped install.

#include "cdc/Geometry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace {

using oa::cdc::kMeshVertexBytes;
using oa::cdc::MeshBounds;
using oa::cdc::MeshVertex;
using oa::cdc::TerrainLeaf;
using oa::cdc::TerrainOctree;
using oa::cdc::TerrainStripChain;

void putI16(std::vector<std::byte>& out, std::int16_t value) {
    const auto raw = static_cast<std::uint16_t>(value);
    for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((raw >> (8u * i)) & 0xFFu));
}

void putU16(std::vector<std::byte>& out, std::uint16_t value) {
    for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((value >> (8u * i)) & 0xFFu));
}

std::vector<std::byte> record(std::int16_t x, std::int16_t y, std::int16_t z, std::uint16_t flag,
                              std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha,
                              std::int16_t texS, std::int16_t texT) {
    std::vector<std::byte> out;
    putI16(out, x);
    putI16(out, y);
    putI16(out, z);
    putU16(out, flag);
    out.push_back(static_cast<std::byte>(red));
    out.push_back(static_cast<std::byte>(green));
    out.push_back(static_cast<std::byte>(blue));
    out.push_back(static_cast<std::byte>(alpha));
    putI16(out, texS);
    putI16(out, texT);
    for (int i = 0; i < 4; ++i) out.push_back(std::byte{0});   // spare word
    EXPECT_EQ(out.size(), kMeshVertexBytes);
    return out;
}

std::vector<std::byte> join(const std::vector<std::vector<std::byte>>& records) {
    std::vector<std::byte> out;
    for (const std::vector<std::byte>& one : records) out.insert(out.end(), one.begin(), one.end());
    return out;
}

// One triangle: a right angle at the origin, 10 units along x and 20 along y.
std::vector<std::byte> triangle() {
    return join({record(0, 0, 0, 1, 0x40, 0x50, 0x60, 0x80, 0x1234, -1),
                 record(10, 0, 0, 1, 0x11, 0x22, 0x33, 0xFF, 0, 0),
                 record(0, 20, 0, 1, 0xAA, 0xBB, 0xCC, 0x00, -4660, 3)});
}

TEST(Mesh, decodesGoldenRecordFields) {
    // A hand-written record: position (16,-10,2), flag 1, colour bytes 40 50 60 80 in file order, texture
    // coordinates 0x1234 and 0xABCD read as signed, spare word zero.
    const std::vector<std::byte> bytes{
        std::byte{0x10}, std::byte{0x00}, std::byte{0xF6}, std::byte{0xFF}, std::byte{0x02}, std::byte{0x00},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x40}, std::byte{0x50}, std::byte{0x60}, std::byte{0x80},
        std::byte{0x34}, std::byte{0x12}, std::byte{0xCD}, std::byte{0xAB}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}};
    std::vector<std::byte> payload = join({bytes, bytes, bytes});

    std::vector<MeshVertex> vertices;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeMesh(payload, &vertices, &error)) << error;
    ASSERT_EQ(vertices.size(), 3u);
    EXPECT_EQ(vertices[0].x, 16);
    EXPECT_EQ(vertices[0].y, -10);
    EXPECT_EQ(vertices[0].z, 2);
    EXPECT_EQ(vertices[0].flag, 1u);
    EXPECT_EQ(vertices[0].red, 0x40u);
    EXPECT_EQ(vertices[0].green, 0x50u);
    EXPECT_EQ(vertices[0].blue, 0x60u);
    EXPECT_EQ(vertices[0].alpha, 0x80u);
    EXPECT_EQ(vertices[0].texS, 0x1234);
    EXPECT_EQ(vertices[0].texT, static_cast<std::int16_t>(0xABCD));
    EXPECT_EQ(vertices[0].pad, 0u);
}

TEST(Mesh, decodesOneTriangle) {
    std::vector<MeshVertex> vertices;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeMesh(triangle(), &vertices, &error)) << error;
    ASSERT_EQ(vertices.size(), 3u);
    EXPECT_EQ(vertices[1].x, 10);
    EXPECT_EQ(vertices[2].y, 20);
    EXPECT_EQ(vertices[2].green, 0xBBu);
    EXPECT_TRUE(error.empty());
}

TEST(Mesh, decodesFromAnOddPayloadOffset) {
    // Archive payloads start at arbitrary byte offsets. Copying per record (rather than reinterpreting the span in
    // place) is what keeps an odd offset from turning into misaligned field reads.
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0x7F});   // one byte of prefix, so every record sits at an odd address
    const std::vector<std::byte> body = triangle();
    payload.insert(payload.end(), body.begin(), body.end());

    std::vector<MeshVertex> vertices;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeMesh(std::span<const std::byte>(payload).subspan(1), &vertices, &error)) << error;
    ASSERT_EQ(vertices.size(), 3u);
    EXPECT_EQ(vertices[0].x, 0);
    EXPECT_EQ(vertices[2].texS, -4660);
}

TEST(Mesh, rejectsTrailingBytes) {
    std::vector<std::byte> payload = triangle();
    payload.push_back(std::byte{0});
    std::vector<MeshVertex> vertices;
    std::string error;
    EXPECT_FALSE(oa::cdc::decodeMesh(payload, &vertices, &error));
    EXPECT_NE(error.find("whole number"), std::string::npos) << error;
}

TEST(Mesh, rejectsPayloadShorterThanATriangle) {
    const std::vector<std::byte> payload(kMeshVertexBytes * 2u + 4u);
    std::vector<MeshVertex> vertices;
    std::string error;
    EXPECT_FALSE(oa::cdc::decodeMesh(payload, &vertices, &error));
    EXPECT_NE(error.find("too short"), std::string::npos) << error;
}

TEST(Mesh, boundsAndTriangleCount) {
    std::vector<MeshVertex> vertices;
    ASSERT_TRUE(oa::cdc::decodeMesh(triangle(), &vertices));
    const MeshBounds bounds = oa::cdc::meshBounds(vertices);
    EXPECT_EQ(bounds.triangles, 1u);
    EXPECT_FLOAT_EQ(bounds.min[0], 0.0f);
    EXPECT_FLOAT_EQ(bounds.max[0], 10.0f);
    EXPECT_FLOAT_EQ(bounds.max[1], 20.0f);
    EXPECT_FLOAT_EQ(bounds.min[2], 0.0f);
}

TEST(Mesh, boundsOfNoVerticesAreZeroed) {
    const MeshBounds bounds = oa::cdc::meshBounds({});
    EXPECT_EQ(bounds.triangles, 0u);
    EXPECT_FLOAT_EQ(bounds.min[0], 0.0f);
    EXPECT_FLOAT_EQ(bounds.max[2], 0.0f);
}


void putU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((value >> (8u * i)) & 0xFFu));
}

// One oct-tree node: the four sphere floats are opaque to the walker, then the +16 reference slot, the child count and
// the self-relative child offsets (GEOMETRY.md §9).
std::vector<std::byte> node(std::uint32_t reference, std::uint32_t childCount, const std::vector<std::uint32_t>& kids) {
    std::vector<std::byte> out;
    for (int i = 0; i < 16; ++i) out.push_back(std::byte{static_cast<std::uint8_t>(i)});
    putU32(out, reference);
    putU32(out, childCount);
    for (std::uint32_t kid : kids) putU32(out, kid);
    return out;
}

TEST(TerrainOctree, walksASyntheticTreeAndKeepsLeafReferences) {
    // Root at 0 with two leaf children at 32 and 56; a leaf carries its reference in the +16 slot, so the map keys are
    // 48 and 72.
    std::vector<std::byte> payload = join({node(0, 2, {32u, 56u}), node(0x1234, 0, {}), node(0x5678, 0, {})});
    ASSERT_EQ(payload.size(), 80u);
    const std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> references{
        {48u, {7u, 480u}},
        {72u, {9u, 824u}},
    };
    TerrainOctree tree;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainOctree(payload, references, &tree, &error)) << error;
    EXPECT_EQ(tree.nodes, 3u);
    EXPECT_EQ(tree.leaves, 2u);
    EXPECT_EQ(tree.maxDepth, 1u);
    ASSERT_EQ(tree.leafList.size(), 2u);
    const auto byNode = [&](std::uint32_t offset) {
        for (const TerrainLeaf& leaf : tree.leafList) {
            if (leaf.nodeOffset == offset) return leaf;
        }
        ADD_FAILURE() << "no leaf at " << offset;
        return TerrainLeaf{};
    };
    EXPECT_EQ(byNode(32u).targetSection, 7u);
    EXPECT_EQ(byNode(32u).offsetInTarget, 480u);
    EXPECT_EQ(byNode(56u).targetSection, 9u);
    EXPECT_EQ(byNode(56u).offsetInTarget, 824u);
}

TEST(TerrainOctree, refusesTreesItCannotTrust) {
    const std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> references{{48u, {7u, 0u}}, {72u, {9u, 8u}}};
    std::string error;
    TerrainOctree tree;

    // A child count outside the measured 0-8 range.
    const auto tooMany = join({node(0, 9, {}), node(0x1234, 0, {}), node(0x5678, 0, {})});
    EXPECT_FALSE(oa::cdc::decodeTerrainOctree(tooMany, references, &tree, &error));
    EXPECT_NE(error.find("outside the measured range"), std::string::npos) << error;

    // Children that are not ascending self-relative offsets.
    const auto unordered = join({node(0, 2, {56u, 32u}), node(0x1234, 0, {}), node(0x5678, 0, {})});
    EXPECT_FALSE(oa::cdc::decodeTerrainOctree(unordered, references, &tree, &error));
    EXPECT_NE(error.find("out of order"), std::string::npos) << error;

    // A leaf whose +16 slot the archive never relocated: inventing a target is worse than refusing.
    const auto unlinked = join({node(0, 2, {32u, 56u}), node(0x1234, 0, {}), node(0x5678, 0, {})});
    EXPECT_FALSE(oa::cdc::decodeTerrainOctree(unlinked, {}, &tree, &error));
    EXPECT_NE(error.find("no relocation reference"), std::string::npos) << error;
}

// One terrain strip record (GEOMETRY.md §10): the header is 44 bytes with vertexCount at +0, matIdx at +20 and
// nextTexture at +40; only those three are read here. The rest of the header stays zero because the decoder does not use it.
std::vector<std::byte> strip(std::uint32_t vertexCount, std::int32_t matIdx, std::uint32_t nextTexture,
                             const std::vector<std::uint16_t>& indices) {
    std::vector<std::byte> out;
    putU32(out, vertexCount);
    for (int i = 0; i < 4; ++i) putU32(out, 0);              // +4 .. +16
    putU32(out, static_cast<std::uint32_t>(matIdx));         // +20
    for (int i = 0; i < 4; ++i) putU32(out, 0);              // +24 .. +36
    putU32(out, nextTexture);                                // +40
    EXPECT_EQ(out.size(), 44u);
    for (std::uint16_t index : indices) putU16(out, index);
    return out;
}

// A vertex array along a straight line, ten units apart. Index runs over it are local and their triangles short, which is
// what the two measured gates in Geometry.hpp look for.
std::vector<MeshVertex> lineVertices(std::size_t count) {
    std::vector<MeshVertex> out;
    for (std::size_t i = 0; i < count; ++i) {
        MeshVertex vertex;
        vertex.x = static_cast<std::int16_t>(10u * i);
        out.push_back(vertex);
    }
    return out;
}

TEST(TerrainStrips, chainsRecordsAndDropsDegenerateRestarts) {
    // A at 0 with four indices chains to B at 52 (44 header + 8 index bytes). The repeated index 2 in A is the restart
    // between two strips of the same run, so it yields no triangle.
    const std::vector<std::byte> payload = join({strip(4, 7, 52u, {0, 1, 2, 2}), strip(3, 9, 0xFFFFFFFFu, {3, 4, 5})});
    TerrainStripChain strips;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChain(payload, lineVertices(8), &strips, &error)) << error;
    EXPECT_EQ(strips.records, 2u);
    EXPECT_EQ(strips.chained, 1u);
    EXPECT_EQ(strips.indices, (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(strips.materialOfTriangle, (std::vector<std::int32_t>{7, 9}));
}

TEST(TerrainStrips, walksACycleOnceInsteadOfHanging) {
    // nextTexture pointing back at the record itself: the walker must claim it once and stop.
    const std::vector<std::byte> payload = strip(3, 1, 0u, {0, 1, 2});
    TerrainStripChain strips;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChain(payload, lineVertices(8), &strips, &error)) << error;
    EXPECT_EQ(strips.records, 1u);
    EXPECT_EQ(strips.indices, (std::vector<std::uint32_t>{0, 1, 2}));
}

TEST(TerrainStrips, yieldsNothingForPayloadsThatHoldNoRecords) {
    TerrainStripChain strips;
    std::string error;

    // A vertex run whose first word is far outside the sane record count.
    const std::vector<std::byte> soup = triangle();
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChain(soup, lineVertices(8), &strips, &error)) << error;
    EXPECT_EQ(strips.records, 0u);
    EXPECT_TRUE(strips.indices.empty());

    // A header-shaped record whose indices name vertices the unit does not have.
    const std::vector<std::byte> outOfRange = strip(3, 2, 0xFFFFFFFFu, {0, 1, 9});
    TerrainStripChain rejected;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChain(outOfRange, lineVertices(8), &rejected, &error)) << error;
    EXPECT_EQ(rejected.records, 0u);
    EXPECT_EQ(rejected.indices.size(), 0u);

    // Header-shaped and inside the index range, but the values jump around: a strip reuses its neighbours, so this fails
    // the locality gate instead of being drawn as spikes across the level.
    const std::vector<std::byte> scattered = strip(4, 3, 0xFFFFFFFFu, {0, 40, 5, 60});
    TerrainStripChain jumpy;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChain(scattered, lineVertices(64), &jumpy, &error)) << error;
    EXPECT_EQ(jumpy.records, 0u);
}

}  // namespace
