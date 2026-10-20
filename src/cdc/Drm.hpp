#pragma once
// DRM unit archive: the payload stored inside one bigfile record (docs/formats/DRM.md).
//
// Anniversary units are version 14. Run over every record of the shipped install with `oa-asset drm`: all 1,098 v14
// units close byte-exactly with every texture section landing on its `PCD9` magic; the census of the rest is in
// docs/formats/CONTAINERS.md §8, and the evidence plus open questions are in docs/formats/DRM.md.

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
    // False when a table entry declares bytes that the image does not store. The last entry of
    // pc-w-hash 8baf540e (level unit) declares 1288 bytes that are absent, and the payloads only
    // close byte-exactly once that entry is treated as memory-only [V-data]. Such an entry has no
    // payload to read; the engine allocates it.
    bool stored = true;
};

// One relocation entry, 8 bytes, stored immediately before its payload. tra.exe walks these in the unit loader
// (FUN_005bca80, case 5): kind is bits 0-2 of the first u16 and the remaining bits name a section **one past** our
// table index - the engine counts sections from 1. Measured on unit `8baf540e`: every one of the 4,776 strip-record
// pointer slots (record +40) and all 622 oct-tree child slots name own-index+1 [V-data], and subtracting one makes
// each of those slots resolve inside its own payload while all 1,620 oct-tree leaf references land on a strip record.
// Kind 0 patches that slot to `base(sectionIndex) + addend`, the addend being the u32 already stored at the slot
// [V-exe]. Other kinds route through the per-type handler objects (kinds 1-3 there, plus a skip default); kind 2
// combines typeSpecific with the low half of the stored value.
struct DrmReloc {
    std::uint16_t kindAndSection = 0;
    std::uint16_t typeSpecific = 0;
    std::uint32_t siteOffset = 0;   // offset of the pointer slot inside the payload
};

inline unsigned char relocKind(const DrmReloc& r) { return static_cast<unsigned char>(r.kindAndSection & 7u); }
// The stored value is one past our directory index, because the engine counts sections from 1 (see DrmReloc above). A
// stored 0 names nothing - the engine never writes it in a real unit - so such an entry resolves to `kNoRelocSection`,
// which callers already handle by bounds-checking the index against the section count.
inline constexpr std::uint32_t kNoRelocSection = 0xFFFFFFFFu;
inline std::uint32_t relocSectionIndex(const DrmReloc& r) {
    const std::uint16_t stored = static_cast<std::uint16_t>(r.kindAndSection >> 3u);
    return stored == 0u ? kNoRelocSection : static_cast<std::uint32_t>(stored - 1u);
}

// One parsed unit. Sections are exposed in table order (the archive stores payloads in that same
// order, back to back with no padding), which is what the engine relies on as well.
class DrmFile {
public:
    // Parses an image obtained from BigfileSet::read. Returns false and fills *error instead of guessing whenever a
    // check fails: version, table bounds inside the image, no candidate payload base that both closes the walk at
    // end-of-image (marking a suffix of directory entries unstored when their bytes were never written) and lands every
    // texture section on its "PCD9" magic, and - only for units with no texture section to anchor on - a pre-payload
    // region below the block size the header declares. Level units need the re-solve; see docs/formats/DRM.md §3.
    // Read-only; the image must outlive this object.
    static bool parse(std::span<const std::byte> image, DrmFile* out, std::string* error = nullptr);

    const DrmHeader& header() const { return header_; }
    std::span<const DrmSection> sections() const { return sections_; }
    std::size_t payloadBase() const { return payloadBase_; }  // where payloads start, derived as in DRM.md §3

    std::span<const std::byte> payload(const DrmSection& section) const {
        if (!section.stored) return {};
        return image_.subspan(section.payloadOffset, section.size);
    }
    std::vector<DrmReloc> relocations(const DrmSection& section) const;

    // The u32 the archive left at a relocation slot: the addend kind 0 adds to the target section's
    // base address (FUN_005bca80 case 5). Zero when the section has no stored payload.
    std::uint32_t relocAddend(const DrmSection& section, const DrmReloc& reloc) const;

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
