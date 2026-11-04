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

// §7.1 [V-data, 2026-10-24] framing: head region {u32 rate, i32 loopStart, u32 samplesPerChannel, u32 channels},
// segment records at 0x800 = {u32 0, u32 dataBytes + 16, u32 0, u32 0, u32 dataBytes, u32 0, u32 0, u32 0} followed by
// whole §3 blocks (mono) / [L][R] block pairs (stereo), chain closing exactly at EOF. The fixtures below are built
// from those measured constants only; the install evidence lives in AUDIO.md §7.1.

namespace {
void putU32At(std::vector<std::uint8_t>& buf, std::size_t at, unsigned v) {
  for (unsigned b = 0u; b < 4u; ++b) buf[at + b] = static_cast<std::uint8_t>((v >> (b * 8u)) & 0xFFu);
}
// One segment record + payload of `blocks` mono blocks (or pairs for stereo, each pair appended as [L][R]).
void appendSegment(std::vector<std::uint8_t>& buf, unsigned blocks, bool stereo) {
  const std::size_t dataBytes = blocks * (stereo ? 72u : 36u);
  const std::size_t at = buf.size();
  buf.resize(at + 32u + dataBytes);
  putU32At(buf, at + 4u, static_cast<unsigned>(dataBytes) + 16u);
  putU32At(buf, at + 16u, static_cast<unsigned>(dataBytes));
  const auto blk = makeAdpcmBlock(1000, 5, 0x31, 0x9F);
  for (unsigned c = 0u; c < blocks; ++c) {
    const std::size_t base = at + 32u + c * (stereo ? 72u : 36u);
    for (std::size_t i = 0u; i < 36u; ++i) buf[base + i] = blk[i];
    if (stereo) for (std::size_t i = 0u; i < 36u; ++i) buf[base + 36u + i] = blk[i];
  }
}
std::span<const std::byte> spanOf(const std::vector<std::uint8_t>& buf) {
  return {reinterpret_cast<const std::byte*>(buf.data()), buf.size()};
}
std::vector<std::uint8_t> mulHead(unsigned rate, int loopStart, unsigned samples, unsigned channels) {
  std::vector<std::uint8_t> buf(0x800u + 32u, 0u);  // head table; record area tail is overwritten by appendSegment
  buf.resize(0x824u, 0u);                            // keep room: first record lands at 0x800
  putU32At(buf, 0u, rate);
  putU32At(buf, 4u, static_cast<unsigned>(loopStart));
  putU32At(buf, 8u, samples);
  putU32At(buf, 12u, channels);
  buf.resize(0x800u);
  return buf;
}
}  // namespace

TEST(MulStream, framesMonoRecordsAndDecodesTrimmedSamples) {
  auto buf = mulHead(44100u, -1, 3u * 64u, 1u);
  appendSegment(buf, 20u, false);   // measured first-run size: 720 payload bytes
  appendSegment(buf, 24u, false);   // measured later-run size: 864 payload bytes
  oa::cdc::MulStream stream;
  ASSERT_TRUE(oa::cdc::frameMulStream(spanOf(buf), &stream));
  EXPECT_EQ(stream.rate, 44100u);
  EXPECT_EQ(stream.loopStartSamples, -1);
  EXPECT_EQ(stream.channels, 1u);
  ASSERT_EQ(stream.segments.size(), 2u);
  EXPECT_EQ(stream.segments[0].first, 0x820u);
  EXPECT_EQ(stream.segments[0].second, 720u);
  EXPECT_EQ(stream.segments[1].first, 0xB10u);  // record at 0xAF0 = 0x800 + 720 + 32; payload follows it
  EXPECT_EQ(stream.segments[1].second, 864u);
  std::vector<std::int16_t> pcm(3u * 64u);
  oa::cdc::decodeMulStream(stream, spanOf(buf), pcm.data());
  // makeAdpcmBlock(1000, 5, 0x31, 0x9F) decodes to the hand-worked values pinned above.
  EXPECT_EQ(pcm[0], 1000);
  EXPECT_EQ(pcm[1], 1010);
  EXPECT_EQ(pcm[64], 1000);   // second block restarts from its own seed
  EXPECT_EQ(pcm[128], 1000);  // third block, second segment
}

TEST(MulStream, framesStereoPairsToFrameInterleavedPcm) {
  auto buf = mulHead(32000u, -1, 64u, 2u);
  appendSegment(buf, 1u, true);  // one [L block][R block] pair = 72 payload bytes
  oa::cdc::MulStream stream;
  ASSERT_TRUE(oa::cdc::frameMulStream(spanOf(buf), &stream));
  EXPECT_EQ(stream.channels, 2u);
  std::vector<std::int16_t> pcm(128u);
  oa::cdc::decodeMulStream(stream, spanOf(buf), pcm.data());
  EXPECT_EQ(pcm[0], 1000);   // L frame 0 seed
  EXPECT_EQ(pcm[1], 1000);   // R frame 0 seed (same fixture block on both channels)
  EXPECT_EQ(pcm[2], 1010);   // L frame 1
}

TEST(MulStream, refusesRecordsThatDoNotCloseTheChain) {
  oa::cdc::MulStream stream;
  { auto bad = mulHead(44100u, -1, 64u, 1u); appendSegment(bad, 20u, false); bad.push_back(0u);  // EOF no longer closes the chain
    EXPECT_FALSE(oa::cdc::frameMulStream(spanOf(bad), &stream)); }
  { auto bad = mulHead(44100u, -1, 64u, 1u); appendSegment(bad, 20u, false); putU32At(bad, 0x800u + 8u, 7u);  // dirty record word
    EXPECT_FALSE(oa::cdc::frameMulStream(spanOf(bad), &stream)); }
  { auto bad = mulHead(44100u, -1, 64u, 3u); appendSegment(bad, 20u, false);  // channels outside the measured {1,2}
    EXPECT_FALSE(oa::cdc::frameMulStream(spanOf(bad), &stream)); }
  { auto bad = mulHead(44100u, -1, 64u, 1u); appendSegment(bad, 20u, false); putU32At(bad, 0x810u, 0x43494e45u);  // CINE voice bag §7.1
    EXPECT_FALSE(oa::cdc::frameMulStream(spanOf(bad), &stream)); }
  { auto bad = mulHead(44100u, -1, 64u, 1u); appendSegment(bad, 20u, false); putU32At(bad, 0x800u + 16u, 720u - 4u);  // payload not whole blocks
    EXPECT_FALSE(oa::cdc::frameMulStream(spanOf(bad), &stream)); }
}

TEST(MulStream, streamsBigEnoughToPassTheWalk) {
  // The gate has no size assumptions between the head table and EOF: a 96 KiB chain of measured-size segments frames.
  auto buf = mulHead(44100u, -1, 2568u * 64u, 1u);
  for (unsigned i = 0u; i < 107u; ++i) appendSegment(buf, 24u, false);  // 107 * (32 + 864) closes at 97,920 = 0x17E80
  EXPECT_EQ(buf.size(), 0x17E80u);
  oa::cdc::MulStream stream;
  ASSERT_TRUE(oa::cdc::frameMulStream(spanOf(buf), &stream));
  EXPECT_EQ(stream.segments.size(), 107u);
}

TEST(MulStream, decodesSegmentsThroughTheBlockPath) {
  // §7.1 decode: a segment's payload is plain §3 blocks; mono emits samples per channel, stereo records interleave
  // [L block][R block] pairs and decodeMulStream de-interleaves them to frames. Hand-worked fixture blocks close the loop.
  const auto seg = [](std::span<const std::uint8_t> payload) {
    // Measured 32-byte segment record: zeros with u32 dataBytes + 16 at +4 and u32 dataBytes at +16 (§7.1).
    std::vector<std::uint8_t> out(32u, 0u);
    putU32At(out, 4u, static_cast<unsigned>(payload.size()) + 16u);
    putU32At(out, 16u, static_cast<unsigned>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
  };
  // Reference series: decode the same block bytes through the shipped §3 path first, then require the .mul segment
  // wiring to reproduce them - mono sample-for-sample, stereo as de-interleaved [L frame][R frame] pairs.
  const auto blockA = makeAdpcmBlock(1000, 5, 0x31, 0x9F);   // the hand-worked ladder fixture from §3's tests
  const auto blockB = makeAdpcmBlock(-500, 7, 0x12, 0xAB);   // a second seed so L and R series differ
  std::vector<std::int16_t> refA(64), refB(64);
  decodeAdpcmBlocks({reinterpret_cast<const std::byte*>(blockA.data()), blockA.size()}, refA.data());
  decodeAdpcmBlocks({reinterpret_cast<const std::byte*>(blockB.data()), blockB.size()}, refB.data());
  // Mono: one segment carrying a single block.
  auto mono = mulHead(44100u, -1, 64u, 1u);
  { auto s = seg(std::span<const std::uint8_t>(blockA.data(), blockA.size()));
    mono.insert(mono.end(), s.begin(), s.end()); }
  oa::cdc::MulStream stream;
  ASSERT_TRUE(oa::cdc::frameMulStream(spanOf(mono), &stream));
  EXPECT_EQ(stream.samplesPerChannel, 64u);
  std::vector<std::int16_t> pcm(64, -7);
  oa::cdc::decodeMulStream(stream, spanOf(mono), pcm.data());
  for (std::size_t i = 0; i < 64u; ++i) EXPECT_EQ(pcm[i], refA[i]) << "mono sample " << i;
  // Stereo: an [L block][R block] pair; frames interleave L then R, each channel on its own §3 stream.
  auto stereo = mulHead(44100u, -1, 64u, 2u);
  { std::vector<std::uint8_t> pair(blockA.size() + blockB.size());
    for (std::size_t i = 0; i < blockA.size(); ++i) pair[i] = blockA[i];
    for (std::size_t i = 0; i < blockB.size(); ++i) pair[blockA.size() + i] = blockB[i];
    auto s = seg(std::span<const std::uint8_t>(pair.data(), pair.size()));
    stereo.insert(stereo.end(), s.begin(), s.end()); }
  ASSERT_TRUE(oa::cdc::frameMulStream(spanOf(stereo), &stream));
  EXPECT_EQ(stream.channels, 2u);
  EXPECT_EQ(stream.samplesPerChannel, 64u);
  std::vector<std::int16_t> frames(128, -7);
  oa::cdc::decodeMulStream(stream, spanOf(stereo), frames.data());
  for (std::size_t i = 0; i < 64u; ++i) {
    EXPECT_EQ(frames[2u * i], refA[i]) << "L frame " << i;      // block A decoded per-channel lands in the L slot
    EXPECT_EQ(frames[2u * i + 1u], refB[i]) << "R frame " << i; }
}
