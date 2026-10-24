#include "cdc/Sound.hpp"

namespace oa {
namespace cdc {

// [V-exe] tra.exe 0x66a288, all 89 entries read and compared word-for-word;
// entries 81..88 exceed 0x1FFF and the engine's startup clamp (FUN_005ebfb0,
// 0x164/4 = 89 entries) replaces them with 0x1FFF, so the stored form below is
// the engine's own post-clamp table. The engine then scales by four for its
// SIMD decode; the scalar form below uses these values unscaled, which the
// prior-art reference does identically.
[[maybe_unused]] static constexpr int kStepLadder[89] = {
    7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8191,  8191,  8191,  8191,  8191,  8191,
    8191,  8191,  8191,  8191,  8191,  8191,  8191,  8191,  8191,
};

// [S] prior-art step-index movement; [V-exe] the running index stays inside
// 0..88 (the ±0x58 clamp in the engine's decode).
static constexpr int kStepMove[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                      -1, -1, -1, -1, 2, 4, 6, 8};

void decodeAdpcmBlocks(std::span<const std::byte> payload,
                       std::int16_t* outSamples) noexcept {
  const std::size_t blocks = payload.size() / 36;
  for (std::size_t b = 0; b < blocks; ++b) {
    const std::byte* p = payload.data() + b * 36;
    std::int16_t* d = outSamples + b * 64;
    // the seed is little-endian i16: sign-extend via int16_t, not uint16_t
    auto predicted = static_cast<std::int32_t>(static_cast<std::int16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1])) << 8) |
        static_cast<std::uint8_t>(p[0])));
    *d++ = static_cast<std::int16_t>(predicted);

    // shortcut: a block whose initial step index is past the ladder is
    // malformed input; it decodes at the top rung. The engine's behaviour for
    // such a block has never been observed.
    int step = static_cast<std::uint8_t>(p[2]) > 88 ? 88 : static_cast<std::uint8_t>(p[2]);

    auto sample = [&](std::uint8_t code) {
      const int rung = kStepLadder[step];
      // The four nibble bits pick the magnitude (step·1/8 · odd 1..15) and a
      // set sign bit negates; written arithmetically, identical to the [S] form.
      int delta = rung >> 3;
      if ((code % 2) != 0) delta += rung >> 2;
      if (((code / 2) % 2) != 0) delta += rung >> 1;
      if (((code / 4) % 2) != 0) delta += rung;
      if (((code / 8) % 2) != 0) delta = -delta;
      predicted += delta;
      if (predicted > 32767) predicted = 32767;
      if (predicted < -32768) predicted = -32768;
      *d++ = static_cast<std::int16_t>(predicted);
      step += kStepMove[code];
      if (step > 88) step = 88;
      if (step < 0) step = 0;
    };

    // [S] the reference skips p[3] and discards the low nibble of p[4]; that
    // quirk is [S], not confirmed in the engine's x86 decode.
    sample(static_cast<std::uint8_t>(p[4]) >> 4);
    for (int i = 0; i < 31; ++i) {
      const std::uint8_t byte = static_cast<std::uint8_t>(p[5 + i]);
      sample(byte & 0x0F);
      sample(byte >> 4);
    }
  }
}

static std::uint32_t le32(const std::byte* at) {
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(at[0]))) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(at[1])) << 8u) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(at[2])) << 16u) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(at[3])) << 24u);
}

// [V-data] docs/formats/AUDIO.md §7: inside a DRM unit, type-6 sections are a twelve-byte {rate, loopStart, loopEnd}
// header plus whole 36-byte blocks of the §3 format - every block's step byte at +2 sits inside the engine's ladder
// clamp (<= 88) and its reserved byte at +3 is zero. The standalone family-1 records never pass this gate (§7's
// negative half), which is why framing here doubles as the discriminator between the two sound families.
bool frameAdpcmStream(std::span<const std::byte> payload, AdpcmStream* out) noexcept {
  if (payload.size() < 12u || (payload.size() - 12u) % 36u != 0u) return false;
  const std::size_t blocks = (payload.size() - 12u) / 36u;
  const std::byte* body = payload.data() + 12u;
  for (std::size_t b = 0u; b < blocks; ++b) {
    if (static_cast<std::uint8_t>(body[b * 36u + 2u]) > 88u || static_cast<std::uint8_t>(body[b * 36u + 3u]) != 0u) return false;
  }
  out->rate = le32(payload.data());
  out->loopStartSamples = static_cast<std::int32_t>(le32(payload.data() + 4u));
  out->loopEndSamples = le32(payload.data() + 8u);
  out->blocks = std::span<const std::byte>(body, blocks * 36u);
  return true;
}

// [V-data] docs/formats/AUDIO.md §7.1 (2026-10-24): the standalone .mul records chain 32-byte segment records from
// offset 0x800; each record is {u32 0, u32 dataBytes + 16, u32 0, u32 0, u32 dataBytes, u32 0, u32 0, u32 0} and its
// payload of whole §3 blocks (mono) or [L block][R] pairs (stereo) starts right after it. Measured on 39/39 named
// records: every chain closes exactly at EOF, payloads are whole multiples of 36 * channels, all 873,808 blocks pass
// the §3 structural test, and the CINE voice-bag variant is refused by its +0x810 magic. The gate keeps every check
// strict so a payload that merely resembles the shape cannot decode into garbage.
bool frameMulStream(std::span<const std::byte> record, MulStream* out) noexcept {
  if (record.size() < 0x820u + 36u) return false;  // head table plus at least one whole block after a record
  const std::uint32_t channels = le32(record.data() + 12u);
  if (channels != 1u && channels != 2u) return false;
  if (le32(record.data() + 0x810u) == 0x43'49'4e'45u) return false;  // CINE voice bag: a different layout, §7.1
  const std::size_t unit = 36u * channels;
  out->rate = le32(record.data());
  out->loopStartSamples = static_cast<std::int32_t>(le32(record.data() + 4u));
  out->samplesPerChannel = le32(record.data() + 8u);
  out->channels = channels;
  out->segments.clear();
  std::size_t p = 0x800u;
  while (true) {
    if (p + 32u > record.size()) return false;
    const std::uint32_t sizePlus16 = le32(record.data() + p + 4u);
    const std::uint32_t dataBytes = le32(record.data() + p + 16u);
    if (sizePlus16 != dataBytes + 16u) return false;
    for (const unsigned off : {0u, 8u, 12u, 20u, 24u, 28u})
      if (le32(record.data() + p + off) != 0u) return false;
    if (dataBytes == 0u || dataBytes % unit != 0u || p + 32u + dataBytes > record.size()) return false;
    out->segments.emplace_back(p + 32u, dataBytes);
    if (p + 32u + dataBytes == record.size()) break;  // [V-data] the chain closes exactly at EOF
    p += dataBytes + 32u;
  }
  return true;
}

// Each segment payload is whole blocks: mono feeds decodeAdpcmBlocks directly; stereo pairs are per-channel block
// runs ([V-data] §7.1: each channel decodes smooth on its own, measured mad/rms 0.03-0.06), emitted frame-
// interleaved to match the engine's stereo PCM contract (§3). The stream is zero-padded to the segment grid; the
// caller's samplesPerChannel bound trims it (measured: blocks * 64 >= samplesPerChannel on every sampled record).
void decodeMulStream(const MulStream& stream, std::span<const std::byte> record,
                     std::int16_t* outSamples) noexcept {
  const std::size_t cap = static_cast<std::size_t>(stream.samplesPerChannel) * stream.channels;
  std::size_t emitted = 0u;
  std::array<std::int16_t, 2u * 64u> pair{};
  for (const auto& [offset, bytes] : stream.segments) {
    const std::span<const std::byte> payload(record.data() + offset, bytes);
    if (stream.channels == 1u) {
      const std::size_t blocks = bytes / 36u;
      for (std::size_t b = 0u; b < blocks && emitted < cap; ++b) {
        decodeAdpcmBlocks(payload.subspan(b * 36u, 36u), pair.data());
        const std::size_t take = std::min<std::size_t>(64u, cap - emitted);
        for (std::size_t i = 0u; i < take; ++i) outSamples[emitted++] = pair[i];
      }
    } else {
      const std::size_t pairs = bytes / 72u;
      for (std::size_t c = 0u; c < pairs && emitted < cap; ++c) {
        decodeAdpcmBlocks(payload.subspan(c * 72u, 36u), pair.data());           // channel L: 64 samples
        decodeAdpcmBlocks(payload.subspan(c * 72u + 36u, 36u), pair.data() + 64u);  // channel R
        for (std::size_t i = 0u; i < 64u && emitted < cap; ++i) {
          outSamples[emitted++] = pair[i];        // L frame
          if (emitted < cap) outSamples[emitted++] = pair[64u + i];  // R frame
        }
      }
    }
  }
}
}  // namespace cdc
}  // namespace oa
