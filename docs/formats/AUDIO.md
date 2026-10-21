# Sound payloads: what is measured, and what is not

Status: **no decoder ships**. The engine's own decode path is located in `tra.exe`, but the payload encoding of the archive
records that carry sound has not been established. Everything below is either read out of the shipped install or read out of
`tra.exe` (sha256 `b43ade0d922b0f007747e5707b97c86404cbf4f051829e3711f7007a656ea53a`); the open question is named at the end.
Evidence tags follow `docs/RESEARCH.md`: **[V-data]** = measured on the install, **[V-exe]** = read from the binary,
**[S]** = prior art / secondary source.

## 1. Two payload families carry sound **[V-data]**

1. **Standalone stream records.** A scan of the first 3,000 archive records whose size sits between 1 KiB and 8 MiB found
   **1,984 payloads that open with a sample-rate dword** (`44100` overwhelmingly; `22050`, `32000`, `36000`, `48000` also
   occur). Their leading fields decode cleanly:

   | offset | field | observed |
   | --- | --- | --- |
   | `+0x00` | u32 sample rate in Hz | 44100 for the great majority |
   | `+0x04` | i32 loop start | `-1` in every record examined (no loop) |
   | `+0x08` | u32 loop end, or length | 2,304 … 7,196,672, i.e. seconds-long loops at these rates |
   | `+0x0C` | u32 channel count? | 1 in the samples read closely; unconfirmed |

   After that header sits a sparse region that is almost entirely zero up to about `+0x800`, then a dense payload whose byte
   density holds near 90 % non-zero from there to the end of the record, with a short zero tail. Record sizes run from 5 KiB
   to 8 MiB, and several hashes appear repeatedly at different specialisation masks (`0xFFFFFFE1`, `0xFFFFF802/4/8/10`), which
   matches the language variants already noted in `docs/formats/CONTAINERS.md` §4.

2. **Samples inside DRM units.** The engine consumes sample bytes from a table whose entries are 52 bytes apart, filled from
   unit sections; see §3.

## 2. How the game opens those streams **[V-exe]**

* `FUN_00419a80(path, type, …)` is the stream factory; the sound callers pass type `0x5a` (90) and `0x46` (70), so two stream
  codecs exist (`FUN_0042db20`, which loops over a prefix table, and `FUN_0042e8b0`, which retries a stored name).
* The prefix table is the pointer array at `0x66508c`: `event_triggered\`, `stingers\`, `montages\`, `animation\`, then the
  literal `default`. Sound stream names are therefore `<prefix>\<name>` hashed through the container's path hash - which is
  why these records have no entry in `objectlist.txt` and stay unnamed in our tooling.
* Movie audio does not go this way: `binkw32.dll` is imported with `_BinkOpenDirectSound`, so Bink plays its own audio, and
  the only winmm users are `waveOutGetNumDevs/GetDevCapsA/Open/PrepareHeader/Write/UnprepareHeader/Reset/Close/GetPosition`.

## 3. The engine's PCM output contract and its ADPCM path **[V-exe]**

* `FUN_0062d720` fills a `WAVEFORMATEX` for `waveOutOpen`: `wFormatTag = 1` (uncompressed PCM), `nChannels = 2`,
  `wBitsPerSample = 16`, `nBlockAlign = 4`, `nAvgBytesPerSec = rate * 4`. So whatever the archive holds, the game turns it
  into **stereo 16-bit PCM** itself before writing.
* The ADPCM path is `FUN_005eca10(src, byteCount, loopStart, loopEnd)`: it computes `blocks = byteCount / 0x24`, allocates
  `blocks * 0x80` bytes of output and calls `FUN_005ec0d0` - **36-byte blocks in, 64 samples out**, exactly the shape of
  cdcEngine's `tr7/cdc/runtime/cdcSound/win32/deADPCM.cpp` **[S]**. `FUN_005eca10` then creates the output sample with a
  length of `blocks << 6` samples and applies the loop points (`FUN_0062d5c0(handle, start, end - 1)`).
* `FUN_005ec0d0` is an unrolled SIMD nibble decoder: precomputed per-byte delta pairs at `0x8b6390`, a step table at
  `0x66a288` that `FUN_005ebfb0` first clamps to `0x1FFF` and then scales by four, running step indices clamped through
  `±0x58` (88). The prior-art scalar form of the same algorithm discards the low nibble of each block's first data byte;
  that quirk has not yet been confirmed in the x86 code.
* Its callers (`FUN_005d1310`, `FUN_005d1260`) walk a runtime table with stride `0x34`: `+0x14` data pointer, `+0x1c` word
  count (byte count is that value doubled), `+0x18` and `+0x20` loop start and end. Those are the fields that feed §3's call.

## 4. What has been ruled out for family 1 **[V-data]**

Measured on extracted payloads (`hash 001AA4B3`, 11,472 bytes, plus header fields from all 1,984 records):

* **Not linear PCM.** Reading the payload as `i16`/`u16`, byte-swapped `u16`, de-interleaved stereo halves, `u8` or `i8` from
  every plausible data start gives a mean sample-to-sample delta of 0.96-1.2 × RMS with a zero-crossing rate near 0.45 -
  white noise, not audio.
* **Not zlib.** No deflate stream inflates from any offset (window bits `15`, `-15`, `31`).
* **No IMA/DVI block framing validates.** For block sizes 36, 40, 68, 72, 132 and 144 with a step-index byte at `+2` (and the
  second channel at `+6`, or four channels at `+2/+6/+10/+14`), no phase over the first 4 KiB keeps those bytes inside the
  legal range `<= 88`: the best fraction across alignments is 0.57 where a real stream must be 1.0 by construction.
* **A whole-install scan does not rescue it.** Walking every one of the 1,098 version-14 DRM units and testing each stored
  section for that same `<= 88` signature returned 22 candidate sections (mostly type 0, sizes 1.4-6 KiB). Decoding them as
  mono IMA - both with and without the prior art's nibble skip - produces sample streams whose discontinuity at every block
  boundary is ~18 × the interior delta, which no encoder produces: those hits are coincidences of a weak signature **[V-data]**.
* `pc-w\globalsoundinfo.drm` section 1 (type 6, 1,092 bytes = exactly 21 × 52) does **not** parse as the runtime stride-`0x34`
  table: every field window tried decodes to implausible values, so the on-disk record layout of that section is unresolved.

## 5. What would close this

Static follow-up on the §2 factory chain (Ghidra, private decompilations): `FUN_00419a80` / `FUN_00419b00` / `FUN_00419b80`
are three one-argument variants of the same creator (`mode` 0/1/2 into `FUN_004198d0`), which builds a name from the
format string `%s%s.mul` against prefix tables at `0x66323c`/`0x663240`, opens it through `FUN_005cefc0`, and attaches a
0x800-byte chunk callback `FUN_00419690` (`FUN_005cf1a0(stream, 0, 0x800, cb, obj)`). So these are the `.mul` sample-bank
streamers - naming and streaming only; no codec arithmetic appears anywhere in the creator chain. The §5 path 1 read
("find the codec inside the soundstream class") is therefore **not** where the codec lives [V-exe]. What remains is
§5 path 2 unchanged: capture `winmm!waveOutWrite` buffers with a dxhook-style pass-through `winmm.dll` inside the prefix,
and diff them against record payloads. That pins the family-1 codec with certainty and simultaneously identifies which
record carries which stream; it is the next audio experiment, and until it lands no audio decoder goes into `src/cdc`.

### 5.1 Original §5 text

Statistics have been exhausted; two concrete evidence paths remain, in order of cost:

1. Read the soundstream class produced by `FUN_00419a80` (its vtable and fill routine) to see which codec converts payload
   bytes into the stereo PCM that §3's `waveOutWrite` feed expects - the two type ids `0x5a`/`0x46` mark exactly where to look.
2. Run the shipped `tra.exe` under Wine with a capture on `winmm!waveOutWrite`, then compare captured buffers against record
   payloads; that pins both the codec and which records carry which stream.

Until one of those lands, no audio decoder goes into `src/cdc`: an unverified guess would silently corrupt every sound in
the game. The ADPCM facts in §3 are recorded so that the first verified implementation can be written directly against them.
