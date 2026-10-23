#include "cdc/Sound.hpp"

#include <array>
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>

using oa::cdc::decodeAdpcmBlocks;
using oa::cdc::makeAdpcmBlock;

// The block bytes come from src/cdc/Sound.hpp's fixture builder so the game can hand the
// same bytes to its device; the expected values below stay worked by hand from the [S]
// scalar form of the engine's verified tables (docs/formats/AUDIO.md §3).

TEST(Sound, decodesAHandWorkedBlock) {
  // seed 1000, step index 5 (ladder rung 12): nibble 3 adds (12>>3)+(12>>2)+(12>>1)=10.
  // step 5 + move[3] = 4 (rung 11): nibble 0xF negates 1+2+5+11=19 -> -19.
  // step 4 + move[0xF] = 12 (rung 23): nibble 9 negates (2+5)=7 -> -7.
  const auto block = makeAdpcmBlock(1000, 5, 0x31, 0x9F);
  std::vector<std::int16_t> out(64);
  decodeAdpcmBlocks({reinterpret_cast<const std::byte*>(block.data()), block.size()}, out.data());
  EXPECT_EQ(out[0], 1000);
  EXPECT_EQ(out[1], 1010);
  EXPECT_EQ(out[2], 991);
  EXPECT_EQ(out[3], 984);
}

TEST(Sound, saturationClampsInsteadOfWrapping) {
  // rung 8191 (post-clamp top): nibble 7 adds 1023+2047+4095+8191=15356 over 32700.
  const auto block = makeAdpcmBlock(32700, 88, 0x70, 0x00);
  std::vector<std::int16_t> out(64);
  decodeAdpcmBlocks({reinterpret_cast<const std::byte*>(block.data()), block.size()}, out.data());
  EXPECT_EQ(out[1], 32767);
  EXPECT_EQ(out[2], 32767);  // stays clamped, does not wrap

  const auto negative = makeAdpcmBlock(-32700, 88, 0xF0, 0x00);
  decodeAdpcmBlocks({reinterpret_cast<const std::byte*>(negative.data()), negative.size()}, out.data());
  EXPECT_EQ(out[1], static_cast<std::int16_t>(-32768));  // floored, not wrapped
  EXPECT_EQ(out[2], static_cast<std::int16_t>(-32768 + 1023));  // recovers off the floor
}

TEST(Sound, ignoresTrailingPartialBlock) {
  // 36 + 4 bytes: the engine's division truncates to one block's 64 samples.
  std::array<std::uint8_t, 40> block{};
  block[0] = 0x11;
  std::vector<std::int16_t> out(64 + 8, 7);
  decodeAdpcmBlocks({reinterpret_cast<const std::byte*>(block.data()), block.size()}, out.data());
  EXPECT_EQ(out[0], 0x11);
  EXPECT_EQ(out[64], 7);  // the partial tail is untouched
}
TEST(Sound, framesTypeSixStreamsAsHeaderPlusWholeBlocks) {
  // AUDIO.md §7 [V-data]: twelve-byte {u32 rate, i32 loopStartSamples, u32 loopEndSamples} then whole 36-byte blocks.
  const auto putU32 = [](std::vector<std::uint8_t>& out, unsigned v) {
      for (unsigned b = 0u; b < 4u; ++b) out.push_back(static_cast<std::uint8_t>((v >> (b * 8u)) & 0xFFu));
  };
  std::vector<std::uint8_t> payload;
  putU32(payload, 44100u);
  putU32(payload, 0xFFFFFFFFu);   // loopStart -1: a non-looping stream
  putU32(payload, 128u);          // two blocks * 64 samples
  for (unsigned i = 0u; i < 2u; ++i) {
      const auto block = makeAdpcmBlock(1000, 5, 0x31, 0x9F);
      payload.insert(payload.end(), block.begin(), block.end());
  }
  oa::cdc::AdpcmStream stream;
  ASSERT_TRUE(oa::cdc::frameAdpcmStream(std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), payload.size()), &stream));
  EXPECT_EQ(stream.rate, 44100u);
  EXPECT_EQ(stream.loopStartSamples, -1);
  EXPECT_EQ(stream.loopEndSamples, 128u);
  EXPECT_EQ(stream.blocks.size(), 72u);
}

TEST(Sound, rejectsPayloadsThatDoNotFrame) {
  // The gate is also the family discriminator: remainder, a step byte past the ladder clamp (88), or a dirty
  // reserved byte at +3 all fail (§7's negative half measures exactly these shapes on standalone records).
  const auto buildWith = [](unsigned blocks, std::uint8_t step, std::uint8_t reserved) {
      std::vector<std::uint8_t> payload(12u, 0u);
      payload[0] = 44100u & 0xFFu;
      payload[1] = (44100u >> 8) & 0xFFu;
      for (unsigned i = 0u; i < blocks; ++i) {
          auto blk = makeAdpcmBlock(1000, step, 0x31, 0x9F);
          auto block = std::vector<std::uint8_t>(blk.begin(), blk.end());
          block[3] = reserved;
          payload.insert(payload.end(), block.begin(), block.end());
      }
      return payload;
  };
  oa::cdc::AdpcmStream stream;
  { auto bad = buildWith(2u, 5u, 0u); bad.push_back(7u); EXPECT_FALSE(oa::cdc::frameAdpcmStream(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bad.data()), bad.size()), &stream)); }
  { auto bad = buildWith(2u, 89u, 0u); EXPECT_FALSE(oa::cdc::frameAdpcmStream(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bad.data()), bad.size()), &stream)); }
  { auto bad = buildWith(2u, 5u, 1u); EXPECT_FALSE(oa::cdc::frameAdpcmStream(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bad.data()), bad.size()), &stream)); }
}
