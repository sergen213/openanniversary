#pragma once
// DRM unit archive: the payload stored inside one bigfile record (docs/formats/DRM.md).
//
// Anniversary units are version 14. Verified against the shipped install with `oa-asset drm`
// (38 of 41 sampled v14 units close byte-exactly with every texture section landing on its magic);
// evidence and open questions live in docs/formats/DRM.md.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oa::cdc {

inline constexpr std::uint32_t kDrmVersionAnniversary = 14;
inline constexpr std::size_t kDrmHeaderBytes = 28;       // v19 siblings use 24: field order differs
inline constexpr std::size_t kDrmSectionEntryBytes = 20;
inline constexpr std::uint32_t kDrmTailType = 32;        // resource-container entry, size 0 in every unit seen

struct DrmHeader {
    std::uint32_t version = 0;
    std::uint32_t numSections = 0;
    std::uint32_t blockABytes = 0;   // numeric block after the section table, not names (DRM.md §3)
    std::uint32_t zero1 = 0;
    std::uint32_t valueD = 0;        // role unknown (DRM.md §4.3)
    std::uint32_t zero2 = 0;
    std::uint32_t sentinel = 0;      // 0xFFFFFFFF in every unit seen
};

struct DrmSection {
    std::uint32_t size = 0;         // payload bytes, excluding the relocation table
    std::uint8_t type = 0;          // 5 = texture ("PCD9"), confirmed here; others see DRM.md §4.4
    std::uint8_t flags = 0;
    std::uint16_t versionId = 0;
    std::uint32_t packed = 0;       // bit0 debug, bits1-7 resource type, bits8-31 relocation count
    std::uint32_t id = 0;
    std::uint32_t specMask = 0xFFFFFFFFu;
    std::uint32_t relocCount = 0;   // derived: packed >> 8, forced to 0 for the size-0 tail entry [V-data]
    std::size_t payloadOffset = 0;  // absolute offset of the payload inside the image
};

struct DrmReloc {
    std::uint32_t offset = 0;       // byte offset inside the payload
    std::uint32_t value = 0;        // patch source, semantics still under investigation
};

// One parsed unit. Sections are exposed in table order (the archive stores payloads in that same
// order, back to back with no padding), which is what the engine relies on as well.
class DrmFile {
public:
    // Parses an image obtained from BigfileSet::read. Returns false and fills *error instead of
    // guessing whenever a check fails: version, table bounds, byte-exact closure at end of image,
    // pre-payload region large enough for the block named by the header, and every texture section
    // starting with "PCD9". Read-only; the image must outlive this object.
    static bool parse(std::span<const std::byte> image, DrmFile* out, std::string* error = nullptr);

    const DrmHeader& header() const { return header_; }
    std::span<const DrmSection> sections() const { return sections_; }
    std::size_t payloadBase() const { return payloadBase_; }  // table end + blockA + extra region

    std::span<const std::byte> payload(const DrmSection& section) const {
        return image_.subspan(section.payloadOffset, section.size);
    }
    std::vector<DrmReloc> relocations(const DrmSection& section) const;

    // Sections whose payloads the parser could anchor-verify (type 5 carries a magic).
    std::size_t anchoredSections() const { return anchored_; }

private:
    DrmHeader header_{};
    std::vector<DrmSection> sections_;
    std::span<const std::byte> image_;
    std::size_t payloadBase_ = 0;
    std::size_t anchored_ = 0;
};

}  // namespace oa::cdc
