#pragma once

// ADPCM sample blocks as the engine decodes them (docs/formats/AUDIO.md §3).
//
// [V-exe] The engine's block driver FUN_005eca10 takes (src, byteCount, loopStart,
// loopEnd), works in whole 36-byte blocks (byteCount / 0x24) and emits 64 samples
// per block; FUN_005ebfb0 initialises the tables at startup (step ladder clamped
// to 0x1FFF then scaled by four, delta pairs built from a 16-entry signed base).
// The 89-entry ladder at 0x66a288 in tra.exe matches the prior-art reference
// word-for-word, so the decode below is the scalar form of the same algorithm,
// kept in the readable [S] form (docs/RESEARCH.md names the reference):
// the engine's SIMD form multiplies the delta pair by the ×4-scaled step through
// pmulhw, which agrees with this form modulo that fixed-point rounding; the
// reference's quirk of skipping a block byte and its low nibble is reproduced
// here and flagged as [S] rather than claimed [V-exe].
//
// The other payload family (the standalone .mul stream records, AUDIO.md §7.1 [V-data, 2026-10-24]) frames as a
// 0x800-byte head region plus self-describing segment records; frameMulStream below gates and decodes them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oa {
namespace cdc {

// Decodes whole 36-byte ADPCM blocks into signed samples, exactly the engine's
// blocks = byteCount / 0x24, samples = blocks * 64 contract. Bytes past the last
// whole block are ignored, as the engine's division truncates. loopStart/loopEnd
// are the loop points the engine hands to its sample handle; this decode keeps
// the whole stream and leaves them to the caller, like the engine does.
// outSamples must hold (payload.size() / 36) * 64 samples, which is what the
// engine allocates for the same call.
void decodeAdpcmBlocks(std::span<const std::byte> payload,
                       std::int16_t* outSamples) noexcept;

// Frames one in-unit type-6 sound stream (docs/formats/AUDIO.md §7 [V-data, 2026-10-23]): a twelve-byte header of
// {u32 rate, i32 loopStartSamples, u32 loopEndSamples} followed by WHOLE 36-byte blocks and nothing else. The gate is
// also the discriminator against the standalone family-1 archive records (§7's negative half: those never close this way),
// so a payload that leaves a remainder after twelve bytes or carries a block failing §3's structural test (step byte at
// +2 inside the engine's ladder clamp of 88, reserved byte at +3 zero) reports false rather than decoding garbage.
struct AdpcmStream {
    std::uint32_t rate = 0;
    std::int32_t loopStartSamples = -1;
    std::uint32_t loopEndSamples = 0;
    std::span<const std::byte> blocks{};
};

bool frameAdpcmStream(std::span<const std::byte> payload, AdpcmStream* out) noexcept;
// Builds one 36-byte block's bytes so tests and the game share the exact fixture: seed little-endian at
// [0..1], step index at [2], [4..5] open the first sample pair; the rest stays zero. Byte [3] is skipped
// by the decoder — the reference quirk marked [S] above.
inline std::array<std::uint8_t, 36> makeAdpcmBlock(std::int16_t seed, std::uint8_t stepIndex,
                                                   std::uint8_t fourth, std::uint8_t fifth) {
    auto block = std::array<std::uint8_t, 36>{};
    block[0] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(seed) & 0xFFu);
    block[1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(seed) >> 8);
    block[2] = stepIndex;
    block[4] = fourth;
    block[5] = fifth;
    return block;
}

// One standalone .mul stream record (AUDIO.md §7.1 [V-data, 2026-10-24], measured on 39/39 named pc-w\sndstrm\*.mul
// records from the vo/event_triggered/stingers/music/ui groups, sizes 5 KiB..17.5 MiB): a head region
// {u32 rate, i32 loopStartSamples(-1), u32 samplesPerChannel, u32 channels(1|2)} (f32 volume/pan parameters at
// +0x34/+0x38 and +0x68), then 0x800 bytes of mostly-zero head table. From 0x800 sits a chain of segment records,
// each 32 bytes {u32 0, u32 dataBytes+16, u32 0, u32 0, u32 dataBytes, u32 0, u32 0, u32 0} immediately followed by
// that many payload bytes = WHOLE §3 blocks: mono records carry 36-byte blocks (measured runs 720/864 bytes =
// 20/24 blocks), stereo records carry per-channel block pairs [L block][R block] = 72 bytes (1440/1728 = 20/24
// pairs). The chain is self-describing — next record = record + dataBytes + 32 — and the last segment's payload ends
// exactly at end of file. Total blocks x 64 >= samplesPerChannel: short streams are zero-padded to the segment grid,
// so the decode trims to samplesPerChannel (measured [V-data]: every sampled record satisfies that bound). The gate
// refuses anything whose chain does not close at EOF with whole-block payloads; the CINE voice-bag variant (§7.1) is
// refused by construction ('CINE' magic at +0x810).
struct MulStream {
    std::uint32_t rate = 0;
    std::int32_t loopStartSamples = -1;
    std::uint32_t samplesPerChannel = 0;   // the +0x08 field, trimmed by decodeMulStream
    std::uint32_t channels = 1;            // measured values: 1 or 2
    std::vector<std::pair<std::size_t, std::size_t>> segments{};  // {payload offset, payload bytes} in file order
};

// Frames a standalone .mul record's payload (the archive record's stored bytes verbatim). Strict: every segment
// header word must be zero except the size pair, sizes must be whole multiples of 36*channels, and the chain must
// end exactly at EOF. [V-data] on the install all 39 sampled standard records pass this gate with 873,808/873,808
// blocks sane under §3's structural test; decoding their streams yields audio (measured mean|delta|/RMS 0.03-0.09,
// zero-crossing rate 0.016-0.034 at 32-44.1 kHz — white noise measures ~1.0 and ~0.45).
bool frameMulStream(std::span<const std::byte> record, MulStream* out) noexcept;

// Decodes every segment of a framed .mul stream into PCM: mono = §3 blocks straight through; stereo = per-channel
// block pairs de-interleaved to frame-interleaved L,R output (measured [V-data]: each channel's block run decodes
// smooth on its own, mad/rms 0.03-0.06). outSamples must hold samplesPerChannel * channels samples; the tail past
// that bound is padding and dropped.
void decodeMulStream(const MulStream& stream, std::span<const std::byte> record,
                     std::int16_t* outSamples) noexcept;

}  // namespace cdc
}  // namespace oa
