# DRM unit files (Anniversary, version 14)

Record payloads extracted from the `bigfile.NNN` containers are **DRM** archives of their own: a section directory
followed by concatenated section payloads. Anniversary uses **version 14**; Underworld (`openunderworld`) documents
**version 19**, whose layout is recorded in that project's `docs/formats/CONTAINERS.md` §4 and is the closest existing
description. This page records what was measured directly against the shipped Anniversary install; every claim carries
a tag: **[V-data]** = reproduced byte-for-byte from extracted files, **[S]** = secondary source, **[I]** = inference.

Evidence corpus (extracted with `oa-asset extract`, read-only):

| File | Size | Sections (`hdr[1]`) | Notes |
| --- | --- | --- | --- |
| `pc-w\lara.drm` | 7,630,042 | 1712 | parses exactly: all 71 texture sections land on their magic, total consumption == file size [V-data] |
| `pc-w\handgun_rbweapon.drm` | 393,702 | 175 | parses exactly (16/16 texture anchors + EOF) [V-data] |
| `pc-w\particle.drm` | 40,766 | 20 | parses exactly (11/11 texture anchors + EOF) [V-data] |
| `pc-w\uigeneral.drm` | 617,651 | 155 | parses exactly once the tail entry is treated per §2.1: 25/25 texture anchors + EOF [V-data] |
| `pc-w\generalbank.drm` | 90,384 | 247 | parses exactly once the tail entry is treated per §2.1: 1/1 texture anchor + EOF [V-data] |
| `pc-w\globalsoundinfo.drm` | 6,998 | 10 | no anchored section type; directory only |

## 1. Header [V-data]

```
u32 version        // 14 in every extracted unit
u32 numSections    // verified: the number of 20-byte entries that makes a walk land on EOF for lara/handgun/particle
u32 blockABytes    // size of the block immediately after the section table (see §3)
u32 zero           // 0 in all six files
u32 valueD         // non-zero, file-dependent: 1792 / 1280 / 2560 / 30208 ... role unknown (§4)
u32 zero           // 0 in all six files
u32 sentinel       // 0xFFFFFFFF in all six files
SectionHeader sections[numSections]   // starts at +28, i.e. this header is 28 bytes, not v19's 24 [V-data]
```

Version check exists in the cdc engine for other titles (`tru.exe` `ResolveReceiver_ReceiveData`, `0x54F9B0`, fatal on
mismatch) [S]; `tra.exe` carries no equivalent string, so Anniversary's check is unconfirmed here. Field order differs
from v19, where the section count sits at index 5 and the table starts at +24 [S].

## 2. Section header — 20 bytes (identical to v19) [V-data]

| Offset | Type | Field | Observed in these six files |
| --- | --- | --- | --- |
| 0 | u32 | payload size | used as the walk step together with §2.1; validated by magic anchors |
| 4 | u8 | section type | `0` (general), `2`, `5` (texture), `6`, `7`, `32` seen |
| 5 | u8 | flags | 0 |
| 6 | u16 | version id | 0 |
| 8 | u32 | packed | bit 0 debug flag (0 everywhere here), bits 1-7 resource type, bits 8-31 relocation entry **count** |
| 12 | u32 | section id | `id == 0` exactly for the type-0 sections [V-data], matching v19's "resource id for non-General sections" [S] |
| 16 | u32 | spec mask | `0xFFFFFFFF` in every section of all six files, so no per-section specialisation filtering is exercised by these units |

Section table entries are sorted by descending payload size (not by id or type) — visible directly at offset +28 [V-data].

### 2.1 Relocation tables [V-data]

Each section is stored as `relocation entries` then `payload`, in section-table order, with **no gap** between
consecutive sections in the three files that walk exactly. For Anniversary one relocation entry is **8 bytes**: taking
the packed upper field as a count and multiplying by 8 makes `lara.drm` land all 71 texture payloads exactly on their
magic and consume the file exactly. (`openunderworld` records v19 storing the same field in *bytes*, TR7 in `count×8`
[S]; our measurement agrees with the TR7 reading.)

**The trailing type-32 entry is not a relocation count.** Every unit examined ends its section table
with one entry of type `32` and payload size `0`. Reading its bits 8-31 as a count makes the walk
over-consume by 56 bytes in `generalbank.drm`, 32 in `uigeneral.drm`, 16/8/8 in three sampled records,
and then no texture magic lands where the directory says. Treating that entry as carrying **no**
relocation table closes all of them byte-exactly with every `PCD9` anchor in place (38 of 41 sampled
v14 units; the other three have no type-5 section to anchor on) [V-data]. Implemented in
`src/cdc/Drm.cpp` (`relocBytes`) and pinned by `tests/test_drm.cpp:parsesTextureGeneralAndTailSentinel`.

### 2.2 Payload magic [V-data]

Type-5 (texture) payloads begin with ASCII `PCD9` followed by a DXT1 encoding tag, e.g. `lara.drm` at `0x92b8`:
`50 43 44 39 … 44 58 54 31`. `tra.exe` validates that magic and fatal-errors otherwise:
`FUN_0060c430` (`decomp_all.c:357269`) compares the first dword with `0x39444350` and calls
`FUN_005df730("Corrupt texture data passed into…")`, then reads width/height/format fields at +0x10, +0x12, +0x15, +0x16
[V-exe]. Those offsets are the starting point for `docs/formats/TEXTURES.md` (not yet written).

## 3. Region between the section table and the first payload [V-data]

After the table comes a block of `blockABytes` bytes whose content is numeric, not ASCII — pairs such as
`(40, 0), (44, 6928), (76, 5776)` in `lara.drm`, i.e. it does **not** hold dependency names the way v19's first name
block does [V-data], unlike v19 where that block holds comma-separated DRM names such as
`g_binoculars, g_grapple…` [S].

Following it there is a further region whose size must be added before payloads start (measured by anchor+EOF solving):

| File | `blockABytes` | extra region |
| --- | --- | --- |
| `lara.drm` | 2368 | 924 |
| `particle.drm` | 140 | 20 (all zero bytes) |
| `handgun_rbweapon.drm` | 288 | 60 |

No header field equals the extra size, so the parser does not try to predict it: it computes the total
of `payload + relocation` bytes over the directory and places the first payload at
`imageSize − total`, which is only valid when that lands past the table end plus `blockABytes` **and**
every texture section then starts with its magic — both are checked, and a unit that fails either is
refused rather than guessed [V-data]. Measured extra sizes: 924 (`lara`), 60 (`handgun_rbweapon`),
36 (`generalbank`), 20 (`particle`, `uigeneral`, `globalsoundinfo`, and one sampled record), 92 (two
sampling records that are spec-mask variants of each other). The remaining unknown is *why* the region
has that size; it no longer blocks parsing.

## 4. Open questions (must stay visible)

1. **Why the extra region (§3) has the size it does.** Parsing works by deriving it from byte-exact
   closure plus magic validation, but no field or rule that predicts it has been found [V-data].
2. ~~Two files disagree with the uniform 8-byte relocation unit~~ — **resolved**: the trailing size-0
   type-32 entry contributes no relocation bytes (§2.1). `generalbank.drm` and `uigeneral.drm` now walk
   exactly, and three sampled records that previously drifted by 8/8/16 bytes also close [V-data].
3. `valueD` (`hdr[4]`) role: values 1280/1792/2560/30208 correlate loosely with file size; unproven.
4. Semantics of section types `32` (present in all six files, exactly one such section each) and `2`,
   `6`, `7`; the v19 type table [S] lists 2 = ANIMATION, 5 = TEXTURE, 6 = WAVE, but Anniversary's
   numbering is only confirmed for 5 here.
5. Relocation entry semantics: pairs of u32 were read directly out of `lara.drm` (offset/value), e.g.
   section 4 at its payload start: `28000000 0c000000 …`; what the value half refers to is unproven.

## 5. Consequences for the implementation

* `src/cdc/Drm.{hpp,cpp}` parses a unit image, exposes sections with their payload offsets and
  relocation entries, and refuses rather than guesses: wrong version, table bounds, closure that does
  not reach end-of-image, a pre-payload region smaller than the header's block A, or a type-5 section
  whose payload does not start with `PCD9`. Verified through `oa-asset drm` on the six units above and
  on 35 sampled records; records that are not DRM units are rejected by version (e.g. `44100`).
* Geometry comes from type-7/type-2 sections of `lara.drm`; textures from type 5 (`PCD9`/DXT1). Both are
  reachable with this parser, so Milestone 4 is unblocked by this page even with §4.1 open.
