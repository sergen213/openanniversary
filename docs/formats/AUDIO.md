# Sound payloads: what is measured, and what is not

Status: **the §3 block-path decoder ships** (`src/cdc/Sound.hpp/.cpp`, tested by `tests/test_sound.cpp`): its step ladder is
compared word-for-word against `tra.exe` **[V-exe]**, while the scalar nibble form follows the prior art (flagged **[S]**
in the header). The payload encoding of the standalone archive records that carry sound (family 1, §1-§2) is still not
established - but sounds inside DRM units (§7) do frame as a twelve-byte header plus whole §3 blocks, so their loader path
is settled. Everything below is either read out of the shipped install or read out of
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

The ADPCM facts in §3 have been written against: §3's block-path decoder now ships in `src/cdc/Sound.*`. For the `.mul` family
nothing unverified has shipped: an unverified guess there would silently corrupt every stream sound, so the winmm capture above
stays the next audio experiment.

## 6. The container's own words and the arithmetic that closes a record **[V-exe/V-data, 2026-10-22]**

Two literals next to each other in the constant pool: `%s%s.mul` at 0x6503b8, and beside `%s%s.raw` at 0x6523a8 the
fragment `%ssndstrm\` — the game opens sound streams as `<prefix>sndstrm\<name>.raw`, a path family the 406 area-code
names never reached. 5,850 candidates (area codes and the 949 objectlist names against `.raw`/`.mul`, and numeric names
0..999 under `sndstrm\`) all hash to zero records: the stream names are not area codes, not object names, not numbers.
Their owner is still unread [V-data].

The size arithmetic of the family-1 reference `001AA4B3` closes exactly against its own header: 15,616 samples = 244
blocks × 64, and 0x800 + 244×36 + 640 = 11,472 = the record's byte size. So the payload IS 244 of the engine's
36-byte blocks plus a 640-byte remainder — but no fixed stride explains it: spread strides 37..47 each keep only
50-60% of the step-byte rule. The 640 bytes are not spread evenly between the blocks [V-data]; a per-block side entry
would be 2.62 bytes, which is not an integer. The framing question stays open, now with the block count pinned
rather than guessed.

The name hunt extended to 13,429 candidates (area codes, object names, numeric and zero-padded names, and the 429
identifier-shaped strings extracted from `cn3`'s two type-10 script sections, each against `pc-w\sndstrm\*.mul`/`.raw`):
all hash to zero records. [V-data] the chain is read though: `FUN_00418720` installs `DAT_00754678` = the startup-built
`<prefix>\sndstrm\`, `FUN_004198d0` opens `%s%s.mul` with that prefix plus a name it does not build itself — a caller
hands it the string, and the interpreter handler `FUN_0042e8b0` copies the name out of a runtime string buffer
(`DAT_007c8870`) that the level scripts fill, then logs `Unable to create soundstream: %s` on failure. So the name IS
a plaintext string in level script data — just not in `cn3`'s two sections.
## 7. Type-6 sections inside DRM units: twelve-byte header plus whole ADPCM blocks **[V-data, 2026-10-23]**

Measured directly on `pc-w\lara.drm` (record hash `5FF91900`, section directory index 24 = payload size 32,736; extracted
through the hex-parsing quirk of GEOMETRY.md §19 by asking for section `18`): the payload is `{u32 rate, i32 loopStart,
u32 loopEnd}` with (size - 12) % 36 == 0 and no leftover — 909 blocks — and every one of those blocks passes §3's structural
test (step byte at +2 <= 88, the engine's ladder clamp, AND reserved byte at +3 == 0). Re-measured across the whole directory
afterwards: every one of lara.drm's 229 type-6 sections closes on that rule with block sanity passing on each of their blocks
(229/229 sizes congruent to 12 mod 36, zero insane blocks), and exactly four of them put `loopEnd` at blocks * 64 samples —
the looping sounds. Inside a unit: sound = twelve-byte header straight into §3's blocks **[V-data]**, gated in code by
`oa::cdc::frameAdpcmStream()` (`src/cdc/Sound.hpp/.cpp`) and played end to end by `--shippedSound='pc-w\lara.drm'` (909 blocks
= 58,176 samples at 44,100 Hz into the platform device; `scripts/ci.sh` asserts that line whenever an install is present).

The standalone archive records of family 1 (§1) are NOT that shape and their framing stays open **[V-data negative]**: `001AA4B3`
(size 11,472) leaves a 12-byte remainder after 318 blocks at +12; the only starts where (size - H) is a whole number of
blocks consistent with the +8 field are the length-derived H = size - (+8 * 9 / 16) ({2,688, 3,248, 3,936, 4,752} on four
sampled records), but blocks read there pass structural sanity on only 17-24% of them. A sweep over every 4-byte start finds
no offset with a sane majority (only trivial two-block tails score high). So standalone family-1 records carry an extra table
or different block layout between header and stream; the loader path for them is unresolved, while the in-unit type-6 path
above is what `FUN_005eca10` receives.

## 8. Type-7 payloads carry the event keys that name the sound-stream banks - 1,034 records get paths **[V-data, V-exe, 2026-10-23]**

The shipped creator chain (`FUN_00419a80`/`FUN_00419b00`/`FUN_00419b80` -> `FUN_004198d0`, AUDIO.md §2/§5) formats
`%s%s.mul` (literal at 0x6503b8) against the prefix table at 0x665090 (`event_triggered\`, `stingers\`, `montages\`,
`animation\`) and the sibling fragment `%ssndstrm\` sits in the same constant pool at 0x6523a8 (§6). The join is what type-7
"name-bearing" payloads store: a single NUL-terminated key like `Event_triggered\time_bend_03.` (the trailing dot belongs to
the key), so the record path is `pc-w\sndstrm\<key>.mul`. `oa-asset namescan` walks every version-fourteen unit's type-7
payloads, harvests those keys, and pairs them with records through the archive's own case-folded hash (CONTAINERS.md §6).

Measured on the install (2026-10-23): **1,098 units scanned, 37,871 type-7 payloads -> 1,329 distinct keys;
matched=1,034, unresolved=295**. Resolved groups: `vo` 728, `event_triggered` 136, `stingers` 90, `music` 58,
`ambience` 9, `animation` 7, `ui` 6. The two keys `ui\TRAE_main_theme_2.` and `ui\trae_main_theme_2.` fold to ONE hash
(pathHash is ASCII case-insensitive), so 1,034 keys name **1,033 distinct records**. Unresolved groups: `vo` 293 plus two
junk runs (`x\`). That closes the naming half of §7's open question for the `.mul` family and cuts CONTAINERS.md §8's
"~3,350 records with no name candidate" by this thousand; textures (`.raw`) and movies (`.bik`/`.sch`) keep their templates open.

What the named banks actually hold stays open **[V-data negative]**: two event banks (`8F2641D7`, `E956B8E6`, 66,048 B) start
`{u32 44100, u32 FFFFFFFF, u32 55552, u32 2}` and a music bank (`C8374111`) starts `{44099, 0, 9592064, 2}` - read as
rate/loopStart/streamLen/flags, the stream is the LAST `streamLen` bytes (gap 10,496 before it). Those tail bytes are NOT §7's
twelve-byte-plus-blocks shape: whole-number-of-36 block sanity passes on only **17.0%** from the tail start and 18.5-18.7% from
offsets 12/16/20, no zlib/gzip/bzip2/xz/PK magic occurs anywhere in the record (zero occurrences of each), entropy is 5.96 bits
in the gap versus 6.45 in the stream, and the last 512 bytes of the gap hold no dword pointing inside the stream. So these banks
carry a table or codec between header and stream that §7's negative half already refuses to guess at; `FUN_00419690`'s chunk
callback (the 0x800-byte reader attached by the same creator) is the next lever, unchanged from §5 path 2.

The unresolved voice keys are shipped references that name no record under `.mul`, `.raw`, or bare decoration: `VO\Lara\B_L_FLL_S_02`
lives in 11 units, first at record index 935 (`35C42F21`) as image offset 6,481,468 of 6,498,009 - inside a float-bearing region,
not a name-bearing type-7 row. That unit carries 200 in-unit type-6 sections (§7's framing) while its clean `VO\Lara\B_LARA_GRT_lift_*`
keys all resolve as standalone banks, so voice lines split between both placements; which of the 295 orphan keys pair with which local
stream is unproven and stays open.

Reproduce: `./build/oa-asset "$G" namescan --out /tmp/names.txt` (stdout = one record row per resolved key, stderr ends with the
summary line above), and the bank heads via `./build/oa-asset "$G" extract --hash 8F2641D7 --mask FFFFFFFF --out /tmp/bank.bin`.
`scripts/ci.sh` asserts a three-digit `matched=` count whenever an install is present.
