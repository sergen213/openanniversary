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

}  // namespace cdc
}  // namespace oa
