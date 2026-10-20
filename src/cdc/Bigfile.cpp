#include "cdc/Bigfile.hpp"

#include <algorithm>
#include <cctype>
#include <zlib.h>

namespace oa::cdc {
namespace {

constexpr std::size_t kMaxParts = 4096;
constexpr std::uint32_t kPolyMsb = 0x04C11DB7u;

bool bigfilePartNumber(const std::string& lowerName, int& number) {
    // "bigfile.000" … "bigfile.999": three-digit decimal suffix, matching the game's "%03d" naming.
    const std::size_t dot = lowerName.rfind('.');
    if (dot == std::string::npos || lowerName.compare(0, dot, "bigfile") != 0) return false;
    if (lowerName.size() - dot - 1 != 3) return false;
    int value = 0;
    for (std::size_t i = dot + 1; i < lowerName.size(); ++i) {
        const char c = lowerName[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    number = value;
    return true;
}

std::uint32_t readU32(const std::byte* p) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24u);
}

bool inflateWith(const std::vector<std::byte>& src, std::vector<std::byte>& dst, int windowBits,
                 std::string* error) {
    z_stream stream{};
    if (inflateInit2(&stream, windowBits) != Z_OK) {
        if (error) *error = "zlib inflateInit failed";
        return false;
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(src.data()));
    stream.avail_in = static_cast<uInt>(src.size());
    stream.next_out = reinterpret_cast<Bytef*>(dst.data());
    stream.avail_out = static_cast<uInt>(dst.size());
    const int code = inflate(&stream, Z_FINISH);
    const bool ok = (code == Z_STREAM_END) && (stream.total_out == dst.size());
    if (!ok && error != nullptr) {
        *error = "zlib inflate failed (code " + std::to_string(code) + ", produced " +
                 std::to_string(stream.total_out) + " of " + std::to_string(dst.size()) + " bytes";
    }
    inflateEnd(&stream);
    return ok;
}

}  // namespace

std::uint32_t pathHash(std::string_view path) {
    std::uint32_t hash = 0xFFFFFFFFu;
    for (const char raw : path) {
        const unsigned char c = (raw >= 'A' && raw <= 'Z')
                                    ? static_cast<unsigned char>(raw - 'A' + 'a')  // _strlwr is ASCII-only
                                    : static_cast<unsigned char>(raw);
        hash ^= static_cast<std::uint32_t>(c) << 24u;
        for (int bit = 0; bit < 8; ++bit) {
            hash = (hash & 0x80000000u) ? ((hash << 1u) ^ kPolyMsb) : (hash << 1u);
        }
    }
    return ~hash;
}

BigfileSet::~BigfileSet() {
    for (const Part& part : parts_) {
        if (part.file != nullptr) std::fclose(part.file);
    }
}

bool BigfileSet::open(const std::filesystem::path& directory, std::string* error) {
    records_.clear();
    for (Part& part : parts_) {
        if (part.file != nullptr) std::fclose(part.file);
    }
    parts_.clear();

    std::vector<std::pair<int, std::filesystem::path>> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        int number = -1;
        if (bigfilePartNumber(name, number)) found.emplace_back(number, entry.path());
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (found.empty()) {
        if (error != nullptr) *error = "no bigfile.NNN parts in " + directory.string();
        return false;
    }
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (found[i].first != static_cast<int>(i)) {
            if (error != nullptr) *error = "bigfile parts are not numbered contiguously from .000";
            return false;
        }
        parts_.push_back({found[i].second, nullptr});
    }
    if (parts_.size() > kMaxParts) {
        if (error != nullptr) *error = "implausible part count " + std::to_string(parts_.size());
        return false;
    }
    if (!openPart(0, error)) return false;

    std::byte header[4];
    if (std::fread(header, 1, sizeof(header), parts_[0].file) != sizeof(header)) {
        if (error != nullptr) *error = "cannot read the index header of " + parts_[0].path.string();
        return false;
    }
    const std::uint32_t count = readU32(header);
    if (count == 0 || count > (1u << 22)) {
        if (error != nullptr) *error = "implausible record count " + std::to_string(count);
        return false;
    }

    const std::uint64_t indexBytes = 4ull + 20ull * count;
    std::vector<std::byte> index(static_cast<std::size_t>(indexBytes));
    if (::fseeko(parts_[0].file, 0, SEEK_SET) != 0 ||
        std::fread(index.data(), 1, static_cast<std::size_t>(indexBytes), parts_[0].file) != indexBytes) {
        if (error != nullptr) *error = "truncated index in " + parts_[0].path.string();
        return false;
    }

    records_.reserve(count);
    const std::byte* hashTable = index.data() + 4;
    const std::byte* recordTable = index.data() + 4 + 4ull * count;
    for (std::uint32_t i = 0; i < count; ++i) {
        Record record;
        record.hash = readU32(hashTable + 4ull * i);
        const std::byte* row = recordTable + 16ull * i;
        record.size = readU32(row);
        record.sector = readU32(row + 4);
        record.specMask = readU32(row + 8);
        record.compressedSize = readU32(row + 12);
        records_.push_back(record);
    }

    // The game binary-searches the hash table, so an unsorted table means we parsed the wrong layout.
    for (std::size_t i = 1; i < records_.size(); ++i) {
        if (records_[i].hash < records_[i - 1].hash) {
            if (error != nullptr) *error = "hash table is not sorted ascending at record " + std::to_string(i);
            return false;
        }
    }
    return true;
}

bool BigfileSet::openPart(std::size_t index, std::string* error) const {
    if (index >= parts_.size()) {
        if (error != nullptr) *error = "part " + std::to_string(index) + " does not exist";
        return false;
    }
    Part& part = parts_[index];
    if (part.file != nullptr) return true;
    part.file = std::fopen(part.path.string().c_str(), "rb");
    if (part.file == nullptr) {
        if (error != nullptr) *error = "cannot open " + part.path.string();
        return false;
    }
    return true;
}

bool BigfileSet::readBytes(std::uint64_t absolute, std::size_t count, std::byte* out, std::string* error) const {
    std::size_t done = 0;
    while (done < count) {
        const std::uint64_t at = absolute + done;
        const auto partIndex = static_cast<std::size_t>(at / partAlignment_);
        const std::uint64_t offset = at % partAlignment_;
        if (!openPart(partIndex, error)) return false;
        if (::fseeko(parts_[partIndex].file, static_cast<off_t>(offset), SEEK_SET) != 0) {
            if (error != nullptr) *error = "seek failed in " + parts_[partIndex].path.string();
            return false;
        }
        const std::size_t want = std::min(count - done, static_cast<std::size_t>(partAlignment_ - offset));
        const std::size_t got = std::fread(out + done, 1, want, parts_[partIndex].file);
        if (got != want) {
            if (error != nullptr) {
                *error = "short read in " + parts_[partIndex].path.string() + " at offset " +
                         std::to_string(offset) + ": wanted " + std::to_string(want) + ", got " +
                         std::to_string(got);
            }
            return false;
        }
        done += want;
    }
    return true;
}

std::optional<std::size_t> BigfileSet::find(std::string_view path, std::uint32_t mask) const {
    if (records_.empty()) return std::nullopt;
    const std::uint32_t target = pathHash(path);
    const auto first = std::lower_bound(records_.begin(), records_.end(), target,
                                        [](const Record& record, std::uint32_t value) {
                                            return record.hash < value;
                                        });
    if (first == records_.end() || first->hash != target) return std::nullopt;

    const auto matches = [&](std::size_t index) { return (records_[index].specMask & mask) == mask; };
    // The shipped search uses the landed slot, then walks the equal-hash window backwards and forwards.
    const std::size_t landed = static_cast<std::size_t>(first - records_.begin());
    if (matches(landed)) return landed;
    for (std::size_t i = landed; i-- > 0;) {
        if (records_[i].hash != target) break;
        if (matches(i)) return i;
    }
    for (std::size_t i = landed + 1; i < records_.size() && records_[i].hash == target; ++i) {
        if (matches(i)) return i;
    }
    return std::nullopt;
}

std::vector<std::byte> BigfileSet::read(std::size_t index, std::string* error) const {
    if (index >= records_.size()) {
        if (error != nullptr) *error = "record index " + std::to_string(index) + " out of range";
        return {};
    }
    const Record& record = records_[index];
    const PartRef ref = splitAtSector(record.sector, partAlignment_);
    const std::uint64_t absolute = ref.part * partAlignment_ + ref.offset;

    if (record.compressedSize == 0) {
        std::vector<std::byte> data(record.size);
        if (!data.empty() && !readBytes(absolute, data.size(), data.data(), error)) return {};
        return data;
    }

    std::vector<std::byte> packed(record.compressedSize);
    if (!readBytes(absolute, packed.size(), packed.data(), error)) return {};
    std::vector<std::byte> data(record.size);
    if (inflateWith(packed, data, 15, error)) return data;

    // Some cdc builds store raw deflate without the zlib header; retry before declaring failure.
    std::string rawError;
    if (inflateWith(packed, data, -15, &rawError)) return data;
    if (error != nullptr) *error += "; as raw deflate: " + rawError;
    return {};
}

}  // namespace oa::cdc
