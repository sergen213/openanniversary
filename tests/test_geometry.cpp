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


TEST(TerrainStrips, walksAChainFromALeafHead) {
    // A at 0 holds three indices and chains to B at 50 (44 header bytes + 6 index bytes); B's link names a zero-count marker
    // header, which is how the shipped data ends a chain (§16). Stored order is kept and repeated indices are drawn as
    // stored - prior art draws them, and TerrainQuery ignores a zero-area triangle rather than tripping over it.
    const std::vector<std::byte> payload = join({strip(3, 7, 50u, {0, 1, 2}), strip(6, 9, 106u, {3, 4, 5, 3, 5, 6}),
                                                 strip(0, -1, 0u, {})});
    const std::uint32_t heads[] = {0u};
    TerrainStripChain walked;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(payload, heads, 64u, &walked, &error)) << error;
    EXPECT_EQ(walked.records, 2u);
    EXPECT_EQ(walked.heads, 1u);
    EXPECT_EQ(walked.badHeads, 0u);
    EXPECT_EQ(walked.brokenLinks, 0u);
    EXPECT_EQ(walked.indices, (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 3, 5, 6}));
    EXPECT_EQ(walked.materialOfTriangle, (std::vector<std::int32_t>{7, 9, 9}));
}

TEST(TerrainStrips, countsAHeadThatIsNotARecordInsteadOfGuessing) {
    // The leaf reference points past the end of this payload. Nothing is invented to cover for it: the head is counted and
    // no triangle comes out. §13/§14's scan-era code did the opposite - it found *some* header-shaped bytes nearby and
    // drew them, which is where the long joins came from.
    const std::vector<std::byte> payload = strip(3, 7, 0u, {0, 1, 2});
    const std::uint32_t heads[] = {4096u};
    TerrainStripChain walked;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(payload, heads, 64u, &walked, &error)) << error;
    EXPECT_EQ(walked.records, 0u);
    EXPECT_EQ(walked.badHeads, 1u);
    EXPECT_TRUE(walked.indices.empty());
}

TEST(TerrainStrips, emitsASharedTailOnce) {
    // Two leaves reach the same chain at different points; the records they share must be drawn once. B at 50 is named by
    // both head 0 (through A's nextTexture) and head 50 itself.
    const std::vector<std::byte> payload = join({strip(3, 7, 50u, {0, 1, 2}), strip(3, 9, 0u, {3, 4, 5})});
    const std::uint32_t heads[] = {0u, 50u};
    TerrainStripChain walked;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(payload, heads, 64u, &walked, &error)) << error;
    EXPECT_EQ(walked.records, 2u);   // A and B, not A+B+B
    EXPECT_EQ(walked.heads, 2u);
    EXPECT_EQ(walked.indices.size(), 6u);
}

TEST(TerrainStrips, refusesARecordNamingVerticesTheUnitDoesNotHave) {
    // A header-shaped start whose index run reaches past the vertex array is not a record of this unit: here it is the head
    // itself that fails, so nothing is drawn and the head is counted as bad.
    const std::vector<std::byte> payload = strip(3, 2, 0u, {0, 1, 9});
    const std::uint32_t heads[] = {0u};
    TerrainStripChain rejected;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(payload, heads, 8u, &rejected, &error)) << error;
    EXPECT_EQ(rejected.records, 0u);
    EXPECT_EQ(rejected.badHeads, 1u);
    EXPECT_TRUE(rejected.indices.empty());

    // The same failure part-way along a chain keeps what came before and names the link that broke.
    const std::vector<std::byte> chained = join({strip(3, 7, 50u, {0, 1, 2}), strip(3, 4, 0u, {3, 4, 9})});
    TerrainStripChain partial;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(chained, heads, 8u, &partial, &error)) << error;
    EXPECT_EQ(partial.records, 1u);
    EXPECT_EQ(partial.brokenLinks, 1u);
    EXPECT_EQ(partial.indices, (std::vector<std::uint32_t>{0, 1, 2}));
}

TEST(TerrainStrips, walksACycleOnceInsteadOfHanging) {
    // B at 56 points back at itself through nextTexture. Without a visited set this loop never ends; with one, both
    // records are drawn once and the chain stops. The six filler bytes put B on an even offset like the shipped data.
    const std::vector<std::byte> payload = join({strip(3, 1, 56u, {0, 1, 2}), std::vector<std::byte>(6),
                                                 strip(3, 2, 56u, {3, 4, 5})});
    const std::uint32_t heads[] = {0u};
    TerrainStripChain walked;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(payload, heads, 64u, &walked, &error)) << error;
    EXPECT_EQ(walked.records, 2u);
    EXPECT_EQ(walked.indices.size(), 6u);
}

TEST(TerrainStrips, endsAChainOnTheZeroCountMarkerAndDoesNotJumpIt) {
    // Chains lie back to back in one payload and a chain's last link names a header whose vertexCount is zero - prior art's
    // loop condition (`tr7/game/pc/D3D/d3dterrain.cpp:117`), measured at offset 0x360 of section 79 in unit `8baf540e`. The
    // marker's own +40 slot points at the next chain, so following it would merge two chains into one; the walk stops at the
    // marker, and the record after it is drawn only when a leaf reference names that record.
    const std::vector<std::byte> payload = join({strip(3, 7, 50u, {0, 1, 2}), strip(0, -1, 100u, {}),
                                                 std::vector<std::byte>(6), strip(3, 9, 0u, {3, 4, 5})});
    const std::uint32_t oneLeaf[] = {0u};
    TerrainStripChain stopped;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(payload, oneLeaf, 64u, &stopped, &error)) << error;
    EXPECT_EQ(stopped.records, 1u);
    EXPECT_EQ(stopped.badHeads, 0u);
    EXPECT_EQ(stopped.brokenLinks, 0u);
    EXPECT_EQ(stopped.indices, (std::vector<std::uint32_t>{0, 1, 2}));

    const std::uint32_t twoLeaves[] = {0u, 100u};   // the second chain has its own leaf reference
    TerrainStripChain both;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(payload, twoLeaves, 64u, &both, &error)) << error;
    EXPECT_EQ(both.records, 2u);
    EXPECT_EQ(both.indices.size(), 6u);
}

TEST(TerrainStrips, yieldsNothingForAPayloadHoldingSomethingElse) {
    // A plain vertex run handed to the chain walker. Its first word packs two i16 coordinates and is not a plausible record
    // count, so the head is refused; had that word been zero the walk would have stopped there honestly - a zero count is the
    // marker prior art's loop tests for, not evidence of a record.
    std::vector<std::byte> soup = triangle();
    for (int i = 0; i < 4; ++i) soup[i] = static_cast<std::byte>((0x4064u >> (8 * i)) & 0xFFu);
    const std::uint32_t heads[] = {0u};
    TerrainStripChain strips;
    std::string error;
    ASSERT_TRUE(oa::cdc::decodeTerrainStripChains(soup, heads, 8u, &strips, &error)) << error;
    EXPECT_EQ(strips.records, 0u);
    EXPECT_EQ(strips.badHeads, 1u);
    EXPECT_TRUE(strips.indices.empty());
}


}  // namespace
