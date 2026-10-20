// Unit tests for the unindexed triangle-run decoder (docs/formats/GEOMETRY.md §2, §6). Fixtures are synthetic: the
// golden record below pins the byte order of every field, and the shape rules come from what a 20-byte stride
// implies for payload sizes measured over the shipped install.

#include "cdc/Geometry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using oa::cdc::kMeshVertexBytes;
using oa::cdc::MeshBounds;
using oa::cdc::MeshVertex;

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

}  // namespace
