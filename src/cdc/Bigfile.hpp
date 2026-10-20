#pragma once
// cdc "bigfile" archive container of the Legend-era engine (Tomb Raider: Anniversary, 2007).
//
// Verified against tra.exe (sha256 b43ade0d922b0f007747e5707b97c86404cbf4f051829e3711f7007a656ea53a)
// and the GOG install of Tomb Raider: Anniversary; see docs/formats/CONTAINERS.md for the evidence.
// Records are addressed only by a hash of their path, so name resolution lives outside this layer.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::cdc {

inline constexpr std::uint32_t kSectorSize = 2048;                 // tra.exe 0x47dbd0: offset * 0x800
inline constexpr std::uint64_t kLegendPartAlignment = 0x9600000ull; // part index = bytes / 0x9600000
inline constexpr std::uint32_t kMaskAll = 0xFFFFFFFFu;

struct Record {
    std::uint32_t hash = 0;            // pathHash of the owning path (from the hash table)
    std::uint32_t size = 0;            // bytes after decompression
    std::uint32_t sector = 0;          // absolute sector index across all parts
    std::uint32_t specMask = kMaskAll;
    std::uint32_t compressedSize = 0;  // 0 = stored uncompressed
};

// Case-folded (ASCII only, like the game's _strlwr) CRC-32: polynomial 0x04C11DB7 written MSB-first,
// init 0xFFFFFFFF, no reflection, final complementation. tra.exe 0x47d970 fed by 0x47da10.
std::uint32_t pathHash(std::string_view path);

struct PartRef {
    std::uint64_t part = 0;
    std::uint64_t offset = 0;
};
constexpr PartRef splitAtSector(std::uint32_t sector, std::uint64_t partAlignment) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(sector) * kSectorSize;
    return {bytes / partAlignment, bytes % partAlignment};
}

class BigfileSet {
public:
    explicit BigfileSet(std::uint64_t partAlignment = kLegendPartAlignment) : partAlignment_(partAlignment) {}
    ~BigfileSet();

    // Finds <dir>/bigfile.NNN (case-insensitive), parses the index of bigfile.000 and opens parts lazily.
    // Returns false and fills *error when the archive is missing or malformed. Never writes anything.
    bool open(const std::filesystem::path& directory, std::string* error = nullptr);

    std::span<const Record> records() const { return records_; }
    std::size_t partCount() const { return parts_.size(); }
    void setSpecMask(std::uint32_t mask) { specMask_ = mask; }
    std::uint32_t currentSpecMask() const { return specMask_; }

    // Binary search over the sorted hash table, then a scan of the equal-hash window for a record whose
    // mask contains every bit of `mask`: (record.specMask & mask) == mask. Mirrors tra.exe 0x47da60.
    std::optional<std::size_t> find(std::string_view path, std::uint32_t mask = kMaskAll) const;

    // Payload of one record, inflated when compressedSize != 0.
    std::vector<std::byte> read(std::size_t index, std::string* error = nullptr) const;

private:
    struct Part {
        std::filesystem::path path;
        mutable std::FILE* file = nullptr;
    };

    bool openPart(std::size_t index, std::string* error) const;
    bool readBytes(std::uint64_t absolute, std::size_t count, std::byte* out, std::string* error) const;

    std::vector<Record> records_;
    mutable std::vector<Part> parts_;
    std::uint64_t partAlignment_ = kLegendPartAlignment;
    std::uint32_t specMask_ = kMaskAll;
};

}  // namespace oa::cdc
