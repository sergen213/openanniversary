// Unit tests for PCD9 texture headers (docs/formats/TEXTURES.md). Fixtures are synthetic; the golden
// numbers come from lara.drm's first texture section (512x512 DXT1 with 10 levels, payloadBytes
// 174776), which is what the mip-chain rule below was measured against.

#include "cdc/Texture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using oa::cdc::Pcd9Format;
using oa::cdc::Pcd9Info;

void putU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((value >> (8u * i)) & 0xFFu));
}

// A PCD9 image: header plus `bodyBytes` of zero-filled block data.
std::vector<std::byte> buildPcd9(std::uint32_t tag, std::uint32_t bodyBytes, std::uint16_t width,
                                std::uint16_t height, std::uint8_t bpp, std::uint8_t levelsMinusOne) {
    std::vector<std::byte> out;
    putU32(out, 0x39444350u);  // 'PCD9'
    putU32(out, tag);
    putU32(out, bodyBytes);
    putU32(out, 0);                                                        // unused field at +12
    out.push_back(static_cast<std::byte>(width & 0xFFu));                  // +16 width low
    out.push_back(static_cast<std::byte>(width >> 8));                     // +17 width high
    out.push_back(static_cast<std::byte>(height & 0xFFu));                 // +18 height low
    out.push_back(static_cast<std::byte>(height >> 8));                    // +19 height high
    out.push_back(static_cast<std::byte>(bpp));                            // +20 bits per pixel
    out.push_back(static_cast<std::byte>(levelsMinusOne));                 // +21 levels-1
    out.push_back(std::byte{0x03});                                        // +22 usage low
    out.push_back(std::byte{0});                                           // +23 usage high
    out.insert(out.end(), bodyBytes, std::byte{0});
    return out;
}

Pcd9Info infoOf(std::uint32_t tag, std::uint16_t w, std::uint16_t h, std::uint8_t bpp, std::uint8_t levels) {
    Pcd9Info info;
    info.tag = tag;
    info.width = w;
    info.height = h;
    info.bitsPerPixel = bpp;
    info.mipLevels = levels;
    return info;
}

TEST(Pcd9, mipChainMatchesLaraGoldenNumbers) {
    // lara.drm section 0: 512x512 DXT1, ten levels; the two smallest levels cost one block each.
    EXPECT_EQ(oa::cdc::pcd9MipChainBytes(infoOf(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 512, 512, 0, 10)),
              174776u);
    // section 7: 256x256 DXT5, nine levels.
    EXPECT_EQ(oa::cdc::pcd9MipChainBytes(infoOf(static_cast<std::uint32_t>(Pcd9Format::Dxt5), 256, 256, 0, 9)),
              87408u);
    // an uncompressed 128x128 ARGB section with no mips.
    EXPECT_EQ(oa::cdc::pcd9MipChainBytes(
                  infoOf(static_cast<std::uint32_t>(Pcd9Format::Uncompressed32), 128, 128, 32, 1)),
              65536u);
}

TEST(Pcd9, parsesDxt1Chain) {
    const std::uint32_t chain = static_cast<std::uint32_t>(oa::cdc::pcd9MipChainBytes(
        infoOf(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 8, 8, 0, 4)));
    ASSERT_EQ(chain, 56u);  // 4 blocks + 1 + 1 + 1 at 8 bytes each
    const auto image = buildPcd9(static_cast<std::uint32_t>(Pcd9Format::Dxt1), chain, 8, 8, 0, 3);
    Pcd9Info info;
    std::string error;
    ASSERT_TRUE(oa::cdc::parsePcd9(image, &info, &error)) << error;
    EXPECT_EQ(info.width, 8u);
    EXPECT_EQ(info.height, 8u);
    EXPECT_EQ(info.mipLevels, 4u);
    EXPECT_EQ(info.payloadBytes, chain);
    EXPECT_EQ(info.usage, 3u);
}

TEST(Pcd9, rejectsUnknownTagAndBadHeaderLength) {
    Pcd9Info info;
    std::string error;
    const auto badTag = buildPcd9(0xDEADBEEFu, 56, 8, 8, 0, 3);
    EXPECT_FALSE(oa::cdc::parsePcd9(badTag, &info, &error));
    EXPECT_NE(error.find("format tag"), std::string::npos) << error;

    const auto shortImage = buildPcd9(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 56, 8, 8, 0, 3);
    EXPECT_FALSE(oa::cdc::parsePcd9(std::vector<std::byte>(shortImage.begin(), shortImage.begin() + 16), &info,
                                    &error));
    EXPECT_NE(error.find("24 bytes"), std::string::npos) << error;
}

TEST(Pcd9, rejectsDeclaredLengthDisagreeingWithSection) {
    auto image = buildPcd9(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 56, 8, 8, 0, 3);
    image.resize(image.size() - 8);  // the section is shorter than the header claims
    Pcd9Info info;
    std::string error;
    EXPECT_FALSE(oa::cdc::parsePcd9(image, &info, &error));
    EXPECT_NE(error.find("declares"), std::string::npos) << error;
}

TEST(Pcd9, rejectsMipCountThatDoesNotFillThePayload) {
    // Same bytes, but claiming one level too few: the chain total no longer equals payloadBytes.
    const auto image = buildPcd9(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 56, 8, 8, 0, 2);
    Pcd9Info info;
    std::string error;
    EXPECT_FALSE(oa::cdc::parsePcd9(image, &info, &error));
    EXPECT_NE(error.find("mips"), std::string::npos) << error;
}

TEST(Pcd9, rejectsUncompressedWithWrongBpp) {
    const auto image = buildPcd9(static_cast<std::uint32_t>(Pcd9Format::Uncompressed32), 64 * 64 * 4, 64, 64, 16,
                                 0);
    Pcd9Info info;
    std::string error;
    EXPECT_FALSE(oa::cdc::parsePcd9(image, &info, &error));
    EXPECT_NE(error.find("bits per pixel"), std::string::npos) << error;
}

}  // namespace

TEST(Pcd9, levelRegionsPackTheChainWithoutGaps) {
    const auto regions = oa::cdc::pcd9LevelRegions(infoOf(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 8, 8, 0, 4));
    ASSERT_EQ(regions.size(), 4u);
    EXPECT_EQ(regions[0].offset, 0u);   // 2x2 blocks of 8 bytes
    EXPECT_EQ(regions[0].bytes, 32u);
    EXPECT_EQ(regions[1].offset, 32u);  // 4x4 texels -> one block
    EXPECT_EQ(regions[1].bytes, 8u);
    EXPECT_EQ(regions[2].offset, 40u);
    EXPECT_EQ(regions[3].offset, 48u);
    std::uint64_t total = 0;
    for (const auto& region : regions) total += region.bytes;
    EXPECT_EQ(total, oa::cdc::pcd9MipChainBytes(infoOf(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 8, 8, 0, 4)));

    // lara.drm section 0: the tail of a 512-wide chain is two single-block levels.
    const auto wide = oa::cdc::pcd9LevelRegions(
        infoOf(static_cast<std::uint32_t>(Pcd9Format::Dxt1), 512, 512, 0, 10));
    ASSERT_EQ(wide.size(), 10u);
    EXPECT_EQ(wide[0].bytes, 131072u);
    EXPECT_EQ(wide[8].offset, 174760u);
    EXPECT_EQ(wide[9].bytes, 8u);
}
