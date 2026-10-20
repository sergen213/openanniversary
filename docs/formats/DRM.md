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
| `pc-w\hash 8baf540e` (unnamed, level unit) | 16,532,812 | 235 | parses once entry 234 is treated as §3.1: 92/92 texture anchors land on `PCD9`, walk closes at file size; payload base 35,600 [V-data] |

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

**Entry fields decoded from the engine.** `tra.exe`'s unit loader (`FUN_005bca80`, case 5) reads each 8-byte entry as
`u16 kindAndSection` (kind in bits 0-2, target section index in bits 3-15), `u16 typeSpecific`, then `u32 siteOffset` —
the byte offset inside the *current* payload where a pointer slot sits. Kind 0 patches that slot to
`base(targetSection) + addend`, and the addend is the u32 the archive already left at the slot [V-exe]. Implemented as
`DrmReloc` plus `relocKind` / `relocSectionIndex` / `relocAddend` (`src/cdc/Drm.{hpp,cpp}`), printed by
`oa-asset drm --section N`. In level unit `8baf540e` all 8,874 relocations are kind 0 and point *across* sections —
the busiest targets are sections 78 (849 references), 87 (623) and 132 (226); the big type-0 payload (section 0,
2,981,540 bytes of vertex records, §GEOMETRY.md) carries no relocation table itself and is referenced only 13 times
[V-data].

### 2.2 Payload magic [V-data]

Type-5 (texture) payloads begin with ASCII `PCD9` followed by a format tag — `'DXT1'`, `'DXT5'` or `0x15` in the units
measured so far — e.g. `lara.drm` at file offset `0x92b8`: `50 43 44 39 … 44 58 54 31`. `tra.exe` validates that magic and
fatal-errors otherwise: `FUN_0060c430` (`decomp_all.c:357269`) compares the first dword with `0x39444350`, calls
`FUN_005df730("Corrupt texture data passed into…")`, then reads width/height/mip count at +0x10, +0x12 and byte +0x15
[V-exe]. The full header and mip-chain rule are worked out in `docs/formats/TEXTURES.md`; `parsePcd9`
(`src/cdc/Texture.cpp`) enforces them and refuses a level count that disagrees with the payload length.

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

No header field equals the extra size, so the parser does not try to predict it: it sums `payload + relocation`
bytes over the directory and places the first payload at `imageSize − total`. That base then has to be confirmed —
every texture section must land on its magic — and two shipped shapes break plain closure [V-data]:

* **A trailing entry declares bytes that were never stored.** Level unit `8baf540e` ends with entry 234 (`type 0`,
  size 1288) whose bytes are absent. Plain closure shifts the base by those 1288 bytes and misanchors all 92 of its
  textures. The parser instead re-solves the base from an actual `PCD9`: what sits ahead of the first texture payload
  is fixed by the earlier entries, including that section's own relocation table. A suffix of directory entries is then
  marked unstored until the walk closes at the image end again; entry 234 reports `stored == false` and yields no
  payload, which is what the engine allocates instead. Pinned by
  `tests/test_drm.cpp:resolvesUnitWithTrailingEntryWhoseBytesAreAbsent`.
* **The declared block A can exceed the room that exists.** Record `F2197DE6` (mask `FFFFFFFF`, 613,514 bytes) stores
  all 13 of its entries with no gap: closure puts the base at 416, i.e. 128 bytes after the table end, while `hdr[2]`
  claims 132 (`0x84`). All nine of its textures anchor there, so the magic anchors decide and that unit parses; units
  with no type-5 payload to anchor on still require `base ≥ tableEnd + hdr[2]`, because closure alone proves nothing
  about where their payloads begin. Pinned by `tests/test_drm.cpp:acceptsAnchoredUnitWhoseRegionIsSmallerThanDeclaredBlockA`
  and `rejectsUnitWithoutAnchorsWhoseRegionIsSmallerThanDeclaredBlockA`.

Measured extra sizes stay as recorded: 924 (`lara`), 60 (`handgun_rbweapon`), 36 (`generalbank`), 20 (`particle`,
`uigeneral`, `globalsoundinfo`, one sampled record), 92 (two spec-mask variants of each other) and 8,292 (`8baf540e`).
*Why* the region has that size is still unknown; it no longer blocks parsing.

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
5. ~~Relocation entry semantics~~ — **resolved** from `tra.exe`'s loader: see §2.1 for the field layout and the kind-0
   addend rule [V-exe]. Kinds 1-3 route through per-type handler objects in that switch; only kind 0 was observed in the
   units measured so far (all 8,874 entries of `8baf540e`, all 57 of `lara.drm` section 4).
6. **What `hdr[2]` really counts.** It matched the region between table end and payloads in four units but exceeds it by
   4 bytes in `F2197DE6`, so it is not simply that byte count [V-data]. The parser uses it only as a floor for units that
   have no magic to anchor on.

## 5. Consequences for the implementation

* `src/cdc/Drm.{hpp,cpp}` parses a unit image, exposes sections with payload offsets, the `stored` flag (§3) and typed
  relocation entries (§2.1), and refuses rather than guesses: wrong version, table bounds, no candidate base that both
  anchors every `PCD9` and closes at the image end, or a pre-payload region below `hdr[2]` when nothing anchors. Run over
  **all 4,304 records** of the shipped install with `oa-asset drm`: all **1,098 version-14 units parse byte-exactly**, and
  every other record is rejected by its leading dword (audio sample rates such as `44100`, `!WAR`, `Mus!`, text) — see
  `docs/formats/CONTAINERS.md` §8 for that census [V-data].
* Textures come from type 5 and are parsed and decoded per `docs/formats/TEXTURES.md` (`src/cdc/Texture.cpp`, exposed by
  `oa-asset texture`). Geometry is documented in `docs/formats/GEOMETRY.md`: the container path reaches every payload of
  every unit, so Milestone 4 is gated only on the geometry layout itself.
