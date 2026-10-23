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
// The other payload family (the standalone .mul stream records) has no decoder
// yet — its codec is still open (AUDIO.md §4/§5); this module decodes the
// block path only.

#include <cstddef>
#include <cstdint>
#include <span>

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

}  // namespace cdc
}  // namespace oa
