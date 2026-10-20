// Unit tests for the cdc bigfile container. The golden hashes below come from the shipped
// Tomb Raider: Anniversary archive (see docs/formats/CONTAINERS.md); synthetic archives cover the
// parsing, mask selection and part-straddling logic without touching game data.

#include "cdc/Bigfile.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

constexpr std::uint64_t kTestAlignment = 4096;  // two sectors per synthetic part

void putU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((value >> (8u * i)) & 0xFFu));
}

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> out;
    for (char c : text) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::byte> compressZlib(std::string_view text, bool rawStream) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, rawStream ? -15 : 15, 9,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        ADD_FAILURE() << "deflateInit2 failed";
        return {};
    }
    std::vector<std::byte> out(deflateBound(&stream, static_cast<uLong>(text.size())) + 64);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    stream.avail_in = static_cast<uInt>(text.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int code = deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    EXPECT_TRUE(code == Z_STREAM_END);
    return out;
}

struct Entry {
    std::string path;
    std::vector<std::byte> storedBytes;  // bytes actually written to the parts
    std::uint32_t logicalSize = 0;       // size after decompression
    std::uint64_t absoluteOffset = 0;
    std::uint32_t specMask = oa::cdc::kMaskAll;
};

// Writes bigfile.000… with the shipped index layout: u32 count, u32 hashes[], then 16-byte records
// (size, sector, specMask, compressedSize). Payloads land at entry.absoluteOffset.
void writeArchive(const std::filesystem::path& dir, std::vector<Entry> entries) {
    std::uint64_t highest = 0;
    for (const Entry& e : entries) {
        const std::uint64_t end = e.absoluteOffset + e.storedBytes.size();
        if (end > highest) highest = end;
    }
    const std::size_t partCount = static_cast<std::size_t>(highest / kTestAlignment) + 1;
    std::vector<std::vector<std::byte>> parts(partCount, std::vector<std::byte>(kTestAlignment, std::byte{0}));

    for (const Entry& e : entries) {
        std::uint64_t at = e.absoluteOffset;
        for (std::byte b : e.storedBytes) {
            const std::size_t part = static_cast<std::size_t>(at / kTestAlignment);
            parts[part][static_cast<std::size_t>(at % kTestAlignment)] = b;
            ++at;
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return oa::cdc::pathHash(a.path) < oa::cdc::pathHash(b.path); });
    std::vector<std::byte> index;
    putU32(index, static_cast<std::uint32_t>(entries.size()));
    for (const Entry& e : entries) putU32(index, oa::cdc::pathHash(e.path));
    for (const Entry& e : entries) {
        const std::uint32_t compressed =
            (e.logicalSize == e.storedBytes.size()) ? 0u : static_cast<std::uint32_t>(e.storedBytes.size());
        putU32(index, e.logicalSize);
        putU32(index, static_cast<std::uint32_t>(e.absoluteOffset / oa::cdc::kSectorSize));
        putU32(index, e.specMask);
        putU32(index, compressed);
    }
    for (std::size_t i = 0; i < index.size(); ++i) parts[0][i] = index[i];

    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::FILE* file = std::fopen((dir / ("bigfile." + std::string(i < 10 ? "00" : "0") + std::to_string(i)))
                                         .string()
                                         .c_str(),
                                     "wb");
        ASSERT_NE(file, nullptr);
        std::fwrite(parts[i].data(), 1, parts[i].size(), file);
        std::fclose(file);
    }
}

class TempArchive : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("oa_bigfile_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    bool open(oa::cdc::BigfileSet& archive, std::string* error = nullptr) const {
        archive = oa::cdc::BigfileSet{kTestAlignment};
        return archive.open(dir_, error);
    }

    std::filesystem::path dir_;
};

std::string textOf(const std::vector<std::byte>& data) {
    std::string out;
    for (std::byte b : data) out.push_back(static_cast<char>(static_cast<unsigned char>(b)));
    return out;
}

}  // namespace

TEST(BigfileHash, MatchesShippedTraPaths) {
    EXPECT_EQ(oa::cdc::pathHash("pc-w\\generalbank.drm"), 0xB3AAAFD6u);
    EXPECT_EQ(oa::cdc::pathHash("pc-w\\lara.drm"), 0x5FF91900u);
    EXPECT_EQ(oa::cdc::pathHash("\\trae\\pc-w\\objectlist.txt"), 0xDD7335E5u);
    EXPECT_EQ(oa::cdc::pathHash("\\trae\\game\\pc\\trae.arg"), 0x6854ECACu);
    // The game lowercases with _strlwr before hashing, so case must not matter.
    EXPECT_EQ(oa::cdc::pathHash("PC-W\\LARA.DRM"), oa::cdc::pathHash("pc-w\\lara.drm"));
}

TEST(BigfileGeometry, SplitsSectorsIntoParts) {
    constexpr std::uint64_t align = 0x9600000ull;
    EXPECT_EQ(oa::cdc::splitAtSector(0, align).part, 0u);
    EXPECT_EQ(oa::cdc::splitAtSector(0, align).offset, 0u);
    const auto lastInPart0 = oa::cdc::splitAtSector((align / oa::cdc::kSectorSize) - 1, align);
    EXPECT_EQ(lastInPart0.part, 0u);
    EXPECT_EQ(lastInPart0.offset, align - oa::cdc::kSectorSize);
    const auto firstInPart1 = oa::cdc::splitAtSector(align / oa::cdc::kSectorSize, align);
    EXPECT_EQ(firstInPart1.part, 1u);
    EXPECT_EQ(firstInPart1.offset, 0u);
}

TEST_F(TempArchive, ReadsStoredAndCompressedRecords) {
    const std::string plain = "hello generalbank";
    const auto longText = std::string(2000, 'Q') + "tail";
    writeArchive(dir_, {
        Entry{"pc-w\\stored.drm", bytesOf(plain), static_cast<std::uint32_t>(plain.size()), 2048},
        Entry{"pc-w\\zlib.drm", compressZlib(longText, false), static_cast<std::uint32_t>(longText.size()), 4096},
        Entry{"pc-w\\rawdefl.drm", compressZlib(longText, true), static_cast<std::uint32_t>(longText.size()), 8192},
    });

    oa::cdc::BigfileSet archive;
    std::string error;
    ASSERT_TRUE(open(archive, &error)) << error;
    ASSERT_EQ(archive.records().size(), 3u);
    EXPECT_GE(archive.partCount(), 3u);

    const auto stored = archive.find("pc-w\\stored.drm");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(textOf(archive.read(*stored)), plain);

    for (const char* name : {"pc-w\\zlib.drm", "pc-w\\rawdefl.drm"}) {
        const auto index = archive.find(name);
        ASSERT_TRUE(index.has_value()) << name;
        EXPECT_GT(archive.records()[*index].compressedSize, 0u);
        EXPECT_EQ(textOf(archive.read(*index)), longText) << name;
    }

    EXPECT_FALSE(archive.find("pc-w\\missing.drm").has_value());
    EXPECT_TRUE(archive.find("PC-W\\STORED.DRM").has_value());  // case-folded lookup
}

TEST_F(TempArchive, ReadsRecordStraddlingPartBoundary) {
    const std::string payload = std::string(1500, 'A') + std::string(1500, 'B');
    // Offset 2048 with a 3000-byte payload crosses the 4096-byte part boundary.
    writeArchive(dir_, {Entry{"pc-w\\straddle.drm", bytesOf(payload), static_cast<std::uint32_t>(payload.size()), 2048}});

    oa::cdc::BigfileSet archive;
    std::string error;
    ASSERT_TRUE(open(archive, &error)) << error;
    const auto index = archive.find("pc-w\\straddle.drm");
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(textOf(archive.read(*index)), payload);
}

TEST_F(TempArchive, SelectsRecordBySpecMask) {
    // Same path twice: one record valid for every configuration (mask 0xFFFFFFFF is a superset of every
    // query), one that only answers when bits 0-3 plus the high bits are set. A third path has two records
    // whose masks are each missing one of the queried bits, so nothing may be returned at all.
    writeArchive(dir_, {
        Entry{"pc-w\\dup.drm", bytesOf("full"), 4, 2048, 0xFFFFFFFFu},
        Entry{"pc-w\\dup.drm", bytesOf("limited"), 7, 4096, 0x0000FFF1u},
        Entry{"pc-w\\pair.drm", bytesOf("bit0only"), 8, 6144, 0xFFFFFFF1u},
        Entry{"pc-w\\pair.drm", bytesOf("nibbles"), 7, 8192, 0xFFFFFFFEu},
    });

    oa::cdc::BigfileSet archive;
    std::string error;
    ASSERT_TRUE(open(archive, &error)) << error;
    ASSERT_EQ(archive.records().size(), 4u);

    // Only the full-mask record contains every bit of this query.
    const auto every = archive.find("pc-w\\dup.drm", 0xFFFFFFFFu);
    ASSERT_TRUE(every.has_value());
    EXPECT_EQ(textOf(archive.read(*every)), "full");

    // Both dup records answer this query; whatever is chosen must satisfy it.
    const auto restricted = archive.find("pc-w\\dup.drm", 0x0000FFF1u);
    ASSERT_TRUE(restricted.has_value());
    EXPECT_EQ(archive.records()[*restricted].specMask & 0x0000FFF1u, 0x0000FFF1u);

    // A full-mask record answers any query, so the same lookup succeeds with unrelated bits.
    const auto arbitrary = archive.find("pc-w\\dup.drm", 0x00000006u);
    ASSERT_TRUE(arbitrary.has_value());
    EXPECT_EQ(textOf(archive.read(*arbitrary)), "full");

    // Neither pair record carries both bits 0 and 1, so this hash must not resolve.
    EXPECT_FALSE(archive.find("pc-w\\pair.drm", 0x00000003u).has_value());
}

TEST_F(TempArchive, RejectsMalformedArchives) {
    std::string error;
    oa::cdc::BigfileSet empty;
    EXPECT_FALSE(open(empty, &error));
    EXPECT_NE(error.find("no bigfile"), std::string::npos) << error;

    writeArchive(dir_, {Entry{"pc-w\\a.drm", bytesOf("x"), 1, 2048},
                        Entry{"pc-w\\b.drm", bytesOf("y"), 1, 4096}});
    std::FILE* file = std::fopen((dir_ / "bigfile.000").string().c_str(), "r+b");
    ASSERT_NE(file, nullptr);
    // Force an inversion in the sorted hash table: count at 0, hashes at 4 and 8.
    std::uint32_t words[3] = {0, 0, 0};
    ASSERT_EQ(::fseeko(file, 0, SEEK_SET), 0);
    ASSERT_EQ(std::fread(words, sizeof(words[0]), 3, file), 3u);
    const std::uint32_t patch = (words[2] >= words[1]) ? 0xFFFFFFFFu : 0u;
    const long slot = (words[2] >= words[1]) ? 4 : 8;
    ASSERT_EQ(::fseeko(file, slot, SEEK_SET), 0);
    ASSERT_EQ(std::fwrite(&patch, sizeof(patch), 1, file), 1u);
    std::fclose(file);

    oa::cdc::BigfileSet broken;
    EXPECT_FALSE(open(broken, &error));
    EXPECT_NE(error.find("not sorted"), std::string::npos) << error;
}

TEST_F(TempArchive, ReportsOutOfRangeReads) {
    writeArchive(dir_, {Entry{"pc-w\\tiny.drm", bytesOf("ab"), 2, 2048}});
    oa::cdc::BigfileSet archive;
    std::string error;
    ASSERT_TRUE(open(archive, &error)) << error;

    EXPECT_TRUE(archive.read(archive.records().size(), &error).empty());
    EXPECT_NE(error.find("out of range"), std::string::npos) << error;
}
