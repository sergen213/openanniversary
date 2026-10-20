// Unit tests for the DRM v14 unit parser (docs/formats/DRM.md). Fixtures are synthetic; the shapes
// mirror measurements taken from shipped units: a texture section with two relocations, a general
// section with one, and the size-0 type-32 tail entry whose packed field must not be read as a
// relocation count (generalbank.drm over-consumes by 56 bytes when it is).

#include "cdc/Drm.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using oa::cdc::DrmFile;
using oa::cdc::DrmSection;

void putU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((value >> (8u * i)) & 0xFFu));
}

void putU8(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void putU32At(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((value >> (8u * i)) & 0xFFu);
}

struct FixtureSection {
    struct DrmRelocStub {
        std::uint32_t offset;
        std::uint32_t value;
    };

    std::uint32_t size;
    std::uint8_t type;
    std::uint32_t packed;  // bits 8-31 hold the relocation count for v14
    std::vector<DrmRelocStub> relocations;
    std::string payload;
};
std::vector<std::byte> buildImage(std::uint32_t version, std::uint32_t blockABytes,
                                 std::uint32_t extraBytes, const std::vector<FixtureSection>& sections) {
    std::vector<std::byte> out;
    putU32(out, version);
    putU32(out, static_cast<std::uint32_t>(sections.size()));
    putU32(out, blockABytes);
    putU32(out, 0);
    putU32(out, 1280);   // valueD: present in every sampled unit, role still unknown
    putU32(out, 0);
    putU32(out, 0xFFFFFFFFu);
    std::vector<std::byte> bodies;
    for (const auto& s : sections) {
        putU32(out, s.size);
        putU8(out, s.type);
        putU8(out, 0);
        out.push_back(std::byte{0});
        out.push_back(std::byte{0});  // versionId (u16)
        putU32(out, s.packed);
        putU32(out, s.type == 0 ? 0u : 0x1234u);  // id
        putU32(out, 0xFFFFFFFFu);                 // specMask
        const bool tail = s.size == 0 && s.type == oa::cdc::kDrmTailType;
        if (tail) {
            if (!s.payload.empty()) ADD_FAILURE() << "the tail entry carries no body in this fixture";
            continue;
        }
        for (const auto& r : s.relocations) {
            putU32(bodies, r.offset);
            putU32(bodies, r.value);
        }
        for (char c : s.payload) bodies.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    // Contiguous table, then block A, then the extra region, then relocation table + payload per
    // section in table order - the storage order measured in shipped units.
    for (std::uint32_t i = 0; i < blockABytes; i += 4) putU32(out, i * 7u);   // numeric, not names
    for (std::uint32_t i = 0; i < extraBytes; ++i) out.push_back(std::byte{0});
    out.insert(out.end(), bodies.begin(), bodies.end());
    return out;
}

std::vector<FixtureSection> standardSections() {
    std::string texture("PCD9DXT1", 8);
    texture.resize(16, '\0');
    return {
        FixtureSection{16, 5, (2u << 8u), {{4u, 0x1111u}, {8u, 0x2222u}}, texture},
        FixtureSection{16, 0, (1u << 8u), {{8u, 0x3333u}}, std::string(16, 'g')},
        // size-0 tail: packed says "7 relocations" and resource type bits set, exactly as in the two
        // shipped units whose walk over-consumed; the parser must still close byte-exactly.
        FixtureSection{0, oa::cdc::kDrmTailType, (7u << 8u) | 84u, {}, {}},
    };
}

TEST(DrmFile, parsesTextureGeneralAndTailSentinel) {
    const auto image = buildImage(14, 8, 20, standardSections());
    DrmFile drm;
    std::string error;
    ASSERT_TRUE(DrmFile::parse(image, &drm, &error)) << error;
    EXPECT_EQ(drm.header().version, 14u);
    EXPECT_EQ(drm.header().numSections, 3u);
    EXPECT_EQ(drm.header().blockABytes, 8u);
    // table end (28 + 60) + block A (8) + extra (20): derived from closure, not stored anywhere.
    EXPECT_EQ(drm.payloadBase(), 116u);
    ASSERT_EQ(drm.sections().size(), 3u);

    const DrmSection& texture = drm.sections()[0];
    EXPECT_EQ(texture.type, 5u);
    EXPECT_EQ(texture.relocCount, 2u);
    EXPECT_EQ(texture.payloadOffset, 116u + 16u);
    ASSERT_EQ(drm.payload(texture).size(), 16u);
    EXPECT_EQ(std::memcmp(drm.payload(texture).data(), "PCD9", 4), 0);
    const auto relocs = drm.relocations(texture);
    ASSERT_EQ(relocs.size(), 2u);
    EXPECT_EQ(relocs[0].offset, 4u);
    EXPECT_EQ(relocs[0].value, 0x1111u);
    EXPECT_EQ(relocs[1].offset, 8u);
    EXPECT_EQ(relocs[1].value, 0x2222u);

    const DrmSection& general = drm.sections()[1];
    EXPECT_EQ(general.relocCount, 1u);
    EXPECT_EQ(general.payloadOffset, 116u + 32u + 8u);
    EXPECT_EQ(drm.relocations(general)[0].value, 0x3333u);

    // The regression this parser exists to get right: the tail's packed upper bits are not a count.
    const DrmSection& tail = drm.sections()[2];
    EXPECT_EQ(tail.type, oa::cdc::kDrmTailType);
    EXPECT_EQ(tail.size, 0u);
    EXPECT_EQ(tail.relocCount, 0u);
    EXPECT_NE(tail.packed >> 8u, 0u);
    EXPECT_EQ(drm.anchoredSections(), 1u);
}

TEST(DrmFile, rejectsMissingTextureMagic) {
    auto sections = standardSections();
    sections[0].payload[1] = 'X';
    const auto image = buildImage(14, 8, 20, sections);
    DrmFile drm;
    std::string error;
    EXPECT_FALSE(DrmFile::parse(image, &drm, &error));
    EXPECT_NE(error.find("PCD9"), std::string::npos) << error;
}

TEST(DrmFile, rejectsOtherVersionsAndTinyImages) {
    DrmFile drm;
    std::string error;
    const auto v19 = buildImage(19, 8, 20, standardSections());
    EXPECT_FALSE(DrmFile::parse(v19, &drm, &error));
    EXPECT_NE(error.find("version"), std::string::npos) << error;

    EXPECT_FALSE(DrmFile::parse(std::vector<std::byte>(8), &drm, &error));
    EXPECT_FALSE(error.empty());
}

TEST(DrmFile, rejectsWalkThatDoesNotCloseAtEndOfImage) {
    // Dropping bytes from a payload shifts the derived base left by that many bytes, so the texture
    // magic no longer lands where the directory says it must.
    auto image = buildImage(14, 8, 20, standardSections());
    image.resize(image.size() - 6);
    DrmFile drm;
    std::string error;
    EXPECT_FALSE(DrmFile::parse(image, &drm, &error));
    EXPECT_TRUE(!error.empty());
}

TEST(DrmFile, rejectsRegionSmallerThanDeclaredBlockA) {
    // The header claims a block A far bigger than the bytes that exist between the table and the
    // payloads, so there is no room for it.
    auto image = buildImage(14, 8, 20, standardSections());
    putU32At(image, 8, 4096u);
    DrmFile drm;
    std::string error;
    EXPECT_FALSE(DrmFile::parse(image, &drm, &error));
    EXPECT_NE(error.find("pre-payload region"), std::string::npos) << error;
}

}  // namespace
