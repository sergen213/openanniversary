#include "cdc/Model.hpp"

#include <array>
#include <cstring>
#include <map>
#include <vector>
#include <gtest/gtest.h>

using oa::cdc::decodeModelMesh;
using oa::cdc::kModelVersionTag;
using oa::cdc::ModelMesh;
using oa::cdc::ModelRecord;
using oa::cdc::parseModelRecord;

// The shipped fixtures reproduce the two lara.drm model headers byte-for-byte, from `od -A x -t x4 /tmp/lara_s4.bin` and
// `/tmp/lara_s6.bin` (extracted with `oa-asset ... drm --path 'pc-w\lara.drm' --section N`). The expected readings come
// from GEOMETRY.md §36's recovered field table, written out by hand.

namespace {

void storeU32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
void storeI16(std::vector<std::byte>& bytes, std::size_t at, std::int16_t value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
void storeU16(std::vector<std::byte>& bytes, std::size_t at, std::uint16_t value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
void storeFloat(std::vector<std::byte>& bytes, std::size_t at, float value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}

std::uint32_t floatBits(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// pc-w\lara.drm section 4 (171,108 bytes), dwords as dumped:
std::vector<std::byte> laraSection4() {
    std::vector<std::byte> bytes(171108, std::byte{0});
    const std::pair<std::size_t, std::uint32_t> fields[] = {
        {0x00, 0x04c20453u}, {0x04, 0x0000006du}, {0x08, 0x00000072u}, {0x0c, 0x000001a0u},
        {0x10, 0x3b89f3e1u}, {0x14, 0x3b89f3e1u}, {0x18, 0x3b89f3e1u}, {0x1c, 0x3f800000u},
        {0x20, 0x00001a17u}, {0x24, 0x00003e00u}, {0x28, 0x00000001u}, {0x2c, 0x0000011cu},
        {0x30, 0x00001f3du}, {0x34, 0x00000000u}, {0x38, 0x00000000u}, {0x3c, 0x43fe9501u},
        {0x40, 0x487d2c04u}, {0x44, 0x00000000u}, {0x48, 0x00000000u}, {0x4c, 0x00006988u},
        {0x50, 0x00000118u}, {0x54, 0x00000114u}, {0x58, 0x0001df70u},
    };
    for (const auto& [at, value] : fields) storeU32(bytes, at, value);
    return bytes;
}

// pc-w\lara.drm section 6 (92,020 bytes), dwords as dumped:
std::vector<std::byte> laraSection6() {
    std::vector<std::byte> bytes(92020, std::byte{0});
    const std::pair<std::size_t, std::uint32_t> fields[] = {
        {0x00, 0x04c20453u}, {0x04, 0x0000006du}, {0x08, 0x0000005du}, {0x0c, 0x000000f0u},
        {0x10, 0x3b858b15u}, {0x14, 0x3b858b15u}, {0x18, 0x3b858b15u}, {0x1c, 0x3f800000u},
        {0x20, 0x000008adu}, {0x24, 0x00003380u}, {0x28, 0x00000001u}, {0x2c, 0x000005ecu},
        {0x30, 0x00000c0au}, {0x34, 0x0000be50u}, {0x38, 0x00000000u}, {0x3c, 0x43fef66fu},
        {0x40, 0x487dedf1u}, {0x44, 0x00000000u}, {0x48, 0x00000000u}, {0x4c, 0x000028b0u},
        {0x50, 0x000005e8u}, {0x54, 0x000005e4u}, {0x58, 0x00011ea0u},
    };
    for (const auto& [at, value] : fields) storeU32(bytes, at, value);
    return bytes;
}

// A miniature model in the shipped layout: two real segments (root pivot z=100 parent -1; child pivot y=10 parent 0),
// one virtual segment (bones 0/1, w=0.5, own pivot z=-50) at index 2 = numSegments; vertices with modelScale 0.5 so
// v0 (i16 pos (0,0,2000), seg 0) lands at world (0, 1100, 0) and v1 ((2000,0,0), seg 1) at (1000, 100, 10); the two
// strips chain at 0x600 -> 0x640 with three indices each. Colours: ARGB red then green.
std::vector<std::byte> miniModel() {
    std::vector<std::byte> bytes(0x700, std::byte{0});
    storeU32(bytes, 0x00, kModelVersionTag);
    storeU32(bytes, 0x04, 2u);                       // numSegments: two real bones
    storeU32(bytes, 0x08, 1u);                       // field08: one more record walked (the virtual) -> 3 segments
    storeU32(bytes, 0x0c, 0x40u);                    // segmentList
    storeFloat(bytes, 0x10, 0.5f); storeFloat(bytes, 0x14, 0.5f); storeFloat(bytes, 0x18, 0.5f);
    storeU32(bytes, 0x20, 2u);                       // numVertices
    storeU32(bytes, 0x24, 0x100u);                   // vertexList
    storeU32(bytes, 0x58, 0x600u);                   // stripList
    // seg0: pivot (0,0,100), parent -1.
    storeFloat(bytes, 0x40u + 0x20u, 0.0f); storeFloat(bytes, 0x40u + 0x24u, 0.0f); storeFloat(bytes, 0x40u + 0x28u, 100.0f);
    storeU32(bytes, 0x40u + 0x38u, 0xFFFFFFFFu);     // parent = -1
    // seg1: pivot (0,10,0), parent 0.
    storeFloat(bytes, 0x80u + 0x24u, 10.0f);
    storeU32(bytes, 0x80u + 0x38u, 0u);
    // seg2 (virtual): bones A=0 B=1 at +0x38/+0x3a, w = 0.5 at +0x3c, own pivot z=-50.
    storeU16(bytes, 0xC0u + 0x38u, 0u); storeI16(bytes, 0xC0u + 0x3au, 1);
    storeFloat(bytes, 0xC0u + 0x3cu, 0.5f);
    storeFloat(bytes, 0xC0u + 0x28u, -50.0f);
    // vertices: 16 bytes each at 0x100.
    storeI16(bytes, 0x100u + 4, 2000); storeI16(bytes, 0x100u + 10, 0);   // v0 pos (0,0,2000) seg 0
    storeI16(bytes, 0x110u + 0, 2000); storeI16(bytes, 0x110u + 10, 1);   // v1 pos (2000,0,0) seg 1
    // strips: header 0x14 then u16 indices; next at +0x10.
    storeI16(bytes, 0x600u + 0, 3); storeU32(bytes, 0x600u + 0x10, 0x640u);
    storeI16(bytes, 0x600u + 0x14, 0); storeI16(bytes, 0x600u + 0x16, 1); storeI16(bytes, 0x600u + 0x18, 0);
    storeI16(bytes, 0x640u + 0, 3);                                        // next left 0: terminates
    storeI16(bytes, 0x640u + 0x14, 1); storeI16(bytes, 0x640u + 0x16, 0); storeI16(bytes, 0x640u + 0x18, 1);
    return bytes;
}

std::vector<std::byte> miniColours() {
    std::vector<std::byte> bytes(8, std::byte{0});
    storeU32(bytes, 0, 0x80FF0000u);   // ARGB: red, alpha 0x80
    storeU32(bytes, 4, 0x8000FF00u);   // ARGB: green
    return bytes;
}

}  // namespace

TEST(Model, parsesTheShippedLaraSection4Header) {
    const auto bytes = laraSection4();
    ModelRecord record;
    std::string error;
    ASSERT_TRUE(parseModelRecord({bytes.data(), bytes.size()}, &record, &error)) << error;
    EXPECT_EQ(record.numSegments, 109u);      // +0x04: the real bones; virtual ones start here (§36)
    EXPECT_EQ(record.field08, 114u);          // draw loop walks 109+114 records [V-exe]
    EXPECT_EQ(record.segmentList, 416u);
    EXPECT_EQ(floatBits(record.modelScale[0]), 0x3b89f3e1u);   // 0.0042099 quantiser, not a sphere centre (§36)
    EXPECT_EQ(record.numVertices, 6679u);     // +0x20: the stride-16 vertex array's count
    EXPECT_EQ(record.vertexList, 15872u);     // +0x24: and its start - 6679*16 later it ends at 122736 = stripList
    EXPECT_EQ(record.stripList, 0x1df70u);    // +0x58: head of the linked triangle-list strips
    EXPECT_EQ(floatBits(record.maxRad), 0x43fe9501u);          // 509.16; +0x40 is its square
}

TEST(Model, parsesTheShippedLaraSection6Header) {
    const auto bytes = laraSection6();
    ModelRecord record;
    std::string error;
    ASSERT_TRUE(parseModelRecord({bytes.data(), bytes.size()}, &record, &error)) << error;
    EXPECT_EQ(record.numSegments, 109u);      // same bone count as section 4 (shared skeleton) [V-data]
    EXPECT_EQ(record.field08, 93u);
    EXPECT_EQ(record.segmentList, 240u);
    EXPECT_EQ(record.numVertices, 2221u);
    EXPECT_EQ(record.vertexList, 13184u);     // +0x24: value is an offset, not an index count (§36's correction)
    EXPECT_EQ(record.stripList, 73376u);
}

TEST(Model, decodesTheMiniModelThroughItsBindPoseAndStrips) {
    const auto bytes = miniModel();
    const auto colours = miniColours();
    ModelMesh mesh;
    std::string error;
    const std::map<std::uint32_t, std::int64_t> noFixups;   // the mini-model stores plain payload-relative offsets
    ASSERT_TRUE(decodeModelMesh({bytes.data(), bytes.size()}, {colours.data(), colours.size()}, noFixups, &mesh, &error)) << error;
    ASSERT_EQ(mesh.vertices.size(), 2u);
    EXPECT_EQ(mesh.vertices[0].x, 0);         // P[seg0] = pivot (0,0,100); local z = 2000*0.5 -> world y = 1100
    EXPECT_EQ(mesh.vertices[0].y, 1100);
    EXPECT_EQ(mesh.vertices[0].z, 0);
    EXPECT_EQ(mesh.vertices[1].x, 1000);      // P[seg1] = (0,10,100) via parent chain; local x -> world x
    EXPECT_EQ(mesh.vertices[1].y, 100);       // model z -> world y: seg1 pivot z inherits 100 from seg0
    EXPECT_EQ(mesh.vertices[1].z, 10);        // model y (pivot 10) -> world z
    EXPECT_EQ(mesh.vertices[0].red, 255);     // ARGB 0x80FF0000 -> r,g,b,a bytes of the stride-20 record
    EXPECT_EQ(mesh.vertices[0].alpha, 0x80);
    EXPECT_EQ(mesh.vertices[1].green, 255);
    EXPECT_FLOAT_EQ(mesh.minY, 100.0f);
    EXPECT_FLOAT_EQ(mesh.maxY, 1100.0f);
    EXPECT_FLOAT_EQ(mesh.height, 1000.0f);
    EXPECT_EQ(mesh.stripsWalked, 2u);
    ASSERT_EQ(mesh.indices.size(), 6u);       // both strips' triangles in walk order
    const std::vector<std::uint32_t> want{0u, 1u, 0u, 1u, 0u, 1u};
    EXPECT_EQ(mesh.indices, want);
}

TEST(Model, virtualSegmentsBlendTheirBonesPositions) {
    // The mini-model's virtual seg2 sits at (1-w)*P[0] + w*(P[1]+pivot): y = 0.5*0 + 0.5*(100-50) in model z -> world y 75?
    // P[0]=(0,0,100), P[1]=(0,10,100); pivot2=(0,0,-50): blend = (0, 5, 100+0.5*(-50)) = model z 75 -> world y 75.
    auto bytes = miniModel();
    storeI16(bytes, 0x110u + 10, 2);   // move v1 onto the virtual segment
    ModelMesh mesh;
    std::string error;
    const std::map<std::uint32_t, std::int64_t> noFixups;
    ASSERT_TRUE(decodeModelMesh({bytes.data(), bytes.size()}, {}, noFixups, &mesh, &error)) << error;
    EXPECT_EQ(mesh.vertices[1].y, 75 + 0);   // blend z=75, local z contribution 0 (v1 pos x-only)
    EXPECT_EQ(mesh.vertices[1].x, 1000);     // local x unaffected by the blend's translation
}

TEST(Model, resolvesStripNextSlotsThroughRelocationFixups) {
    // The shipped shape (lara section 4): every `next` slot holds a relocation addend, not an offset. Scrambling the
    // mini-model's slot to garbage and handing the decoder the resolved target reproduces the walk; a fixup that names
    // another section (-1) ends the chain at that strip instead of following junk.
    auto bytes = miniModel();
    storeU32(bytes, 0x600u + 0x10u, 0xDEADBEEFu);   // first strip's stored slot: raw walk would leave the payload
    const std::map<std::uint32_t, std::int64_t> fixups{{0x600u + 0x10u, 0x640}};
    ModelMesh mesh;
    std::string error;
    ASSERT_TRUE(decodeModelMesh({bytes.data(), bytes.size()}, {}, fixups, &mesh, &error)) << error;
    EXPECT_EQ(mesh.stripsWalked, 2u);
    EXPECT_EQ(mesh.indices.size(), 6u);

    const std::map<std::uint32_t, std::int64_t> leaves{{0x600u + 0x10u, -1}};
    ModelMesh cut;
    ASSERT_TRUE(decodeModelMesh({bytes.data(), bytes.size()}, {}, leaves, &cut, &error)) << error;
    EXPECT_EQ(cut.stripsWalked, 1u);
    EXPECT_EQ(cut.indices.size(), 3u);
}

TEST(Model, rejectsAForeignTag) {
    auto bytes = laraSection4();
    storeU32(bytes, 0x00, 0x39444350u);  // PCD9: a render resource tag, not the model version the exe checks at 0x5db249
    ModelRecord record;
    std::string error;
    EXPECT_FALSE(parseModelRecord({bytes.data(), bytes.size()}, &record, &error));
    EXPECT_NE(error.find("0x04c20453"), std::string::npos) << error;  // the exe's own expected value in the message
}

TEST(Model, rejectsArraysThatDoNotFitThePayload) {
    auto bytes = laraSection6();
    storeU32(bytes, 0x20, 99999u);  // vertex array: 99999*16 from 13184 runs far past 92,020
    ModelRecord record;
    std::string error;
    EXPECT_FALSE(parseModelRecord({bytes.data(), bytes.size()}, &record, &error));
    EXPECT_NE(error.find("vertex array"), std::string::npos) << error;

    storeU32(bytes, 0x20, 2221u);
    storeU32(bytes, 0x58, 999999u);  // strip head outside the payload
    EXPECT_FALSE(parseModelRecord({bytes.data(), bytes.size()}, &record, &error));
    EXPECT_NE(error.find("strip list head"), std::string::npos) << error;
}

TEST(Model, rejectsAPayloadShorterThanTheHeader) {
    std::array<std::byte, 0x40> tiny{};
    ModelRecord record;
    std::string error;
    EXPECT_FALSE(parseModelRecord({tiny.data(), tiny.size()}, &record, &error));
    EXPECT_FALSE(error.empty());
}
