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
        std::uint32_t kindAndSection;  // low u16 of the entry: bits 0-2 kind, bits 3-15 section index
        std::uint32_t siteOffset;      // high u16 carries typeSpecific, folded in here
    };

    std::uint32_t size;
    std::uint8_t type;
    std::uint32_t packed;  // bits 8-31 hold the relocation count for v14
    std::vector<DrmRelocStub> relocations;
    std::string payload;
    bool absent = false;   // entry declares bytes the image does not store (level units do this)
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
        if (s.absent) continue;  // table entry with no stored body
        for (const auto& r : s.relocations) {
            putU32(bodies, r.kindAndSection);
            putU32(bodies, r.siteOffset);
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
    // Relocation words follow the loader's encoding (FUN_005bca80 case 5): kind in bits 0-2 of the
    // first u16, target section index in bits 3-15, typeSpecific in the second u16.
    return {
        FixtureSection{16, 5, (2u << 8u), {{(1u << 3u) | 0u, 4u}, {(0u << 3u) | 2u | (3u << 16u), 8u}}, texture},
        FixtureSection{16, 0, (1u << 8u), {{(5u << 3u) | 1u, 8u}}, std::string(16, 'g')},
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
    EXPECT_EQ(oa::cdc::relocKind(relocs[0]), 0u);
    EXPECT_EQ(oa::cdc::relocSectionIndex(relocs[0]), 1u);
    EXPECT_EQ(relocs[0].typeSpecific, 0u);
    EXPECT_EQ(relocs[0].siteOffset, 4u);
    // Kind 0 adds the u32 sitting at its own slot; here that is the ASCII "DXT1" the fixture stored.
    EXPECT_EQ(drm.relocAddend(texture, relocs[0]), 0x31545844u);
    EXPECT_EQ(oa::cdc::relocKind(relocs[1]), 2u);
    EXPECT_EQ(oa::cdc::relocSectionIndex(relocs[1]), 0u);
    EXPECT_EQ(relocs[1].typeSpecific, 3u);
    EXPECT_EQ(relocs[1].siteOffset, 8u);

    const DrmSection& general = drm.sections()[1];
    EXPECT_EQ(general.relocCount, 1u);
    EXPECT_EQ(general.payloadOffset, 116u + 32u + 8u);
    const auto generalRelocs = drm.relocations(general);
    ASSERT_EQ(generalRelocs.size(), 1u);
    EXPECT_EQ(oa::cdc::relocKind(generalRelocs[0]), 1u);
    EXPECT_EQ(oa::cdc::relocSectionIndex(generalRelocs[0]), 5u);

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

TEST(DrmFile, rejectsUnitWithoutAnchorsWhoseRegionIsSmallerThanDeclaredBlockA) {
    // Nothing anchors these payloads on a magic, so closure alone proves nothing about where they
    // begin; the block A the header declares still has to fit between table end and that base.
    const std::vector<FixtureSection> sections{
        FixtureSection{16, 0, (1u << 8u), {{(3u << 3u) | 0u, 4u}}, std::string(16, 'g')},
        FixtureSection{0, oa::cdc::kDrmTailType, 0u, {}, {}},
    };
    auto image = buildImage(14, 8, 20, sections);
    putU32At(image, 8, 4096u);
    DrmFile drm;
    std::string error;
    EXPECT_FALSE(DrmFile::parse(image, &drm, &error));
    EXPECT_NE(error.find("pre-payload region"), std::string::npos) << error;
}

TEST(DrmFile, acceptsAnchoredUnitWhoseRegionIsSmallerThanDeclaredBlockA) {
    // pc-w-hash F2197DE6 stores every payload with no gap and closes at the image end from a base
    // whose pre-payload room is 4 bytes below what hdr[2] declares (132 declared, 128 present), while
    // all nine of its textures land on PCD9. The magic anchors are the stronger evidence: same shape
    // here - three entries, base 116, room 28 bytes, header claiming 32.
    auto image = buildImage(14, 8, 20, standardSections());
    putU32At(image, 8, 32u);
    DrmFile drm;
    std::string error;
    ASSERT_TRUE(DrmFile::parse(image, &drm, &error)) << error;
    EXPECT_EQ(drm.payloadBase(), 116u);
    EXPECT_EQ(drm.anchoredSections(), 1u);
    EXPECT_EQ(std::memcmp(drm.payload(drm.sections()[0]).data(), "PCD9", 4), 0);
}

TEST(DrmFile, resolvesUnitWithTrailingEntryWhoseBytesAreAbsent) {
    // Shipped level units (pc-w-hash 8baf540e) declare a final entry whose bytes the image never
    // stores. Closure alone then shifts the base and misanchors every texture; the parser must
    // re-anchor on an actual PCD9 and mark that entry unstored instead of rejecting the unit.
    auto sections = standardSections();
    sections.push_back(FixtureSection{24, 10, 5u, {}, {}, /*absent=*/true});
    const auto image = buildImage(14, 8, 20, sections);

    DrmFile drm;
    std::string error;
    ASSERT_TRUE(DrmFile::parse(image, &drm, &error)) << error;
    ASSERT_EQ(drm.sections().size(), 4u);
    EXPECT_EQ(drm.payloadBase(), 28u + 4u * 20u + 8u + 20u);
    EXPECT_EQ(drm.sections()[0].payloadOffset, drm.payloadBase() + 16u);
    EXPECT_EQ(std::memcmp(drm.payload(drm.sections()[0]).data(), "PCD9", 4), 0);
    EXPECT_EQ(drm.anchoredSections(), 1u);
    EXPECT_TRUE(drm.sections()[2].stored);
    EXPECT_FALSE(drm.sections()[3].stored);
    EXPECT_TRUE(drm.payload(drm.sections()[3]).empty());
    EXPECT_EQ(drm.sections()[1].payloadOffset, drm.payloadBase() + 32u + 8u);
    EXPECT_EQ(static_cast<unsigned char>(drm.payload(drm.sections()[1])[0]), 'g');
}

}  // namespace
