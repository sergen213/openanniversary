# PCD9 textures (Anniversary, DRM v14 type-5 payloads)

Every field below is measured from the 71 type-5 sections of `pc-w\lara.drm` (record hash `0x5FF91900`)
**[V-data]**, or read out of the engine's texture constructor `FUN_0060c430`
(`openanniversary-work/ghidra/exports/decomp_all.c:357269`) **[V-exe]**. Reproduce with
`oa-asset drm` and `oa-asset texture`, commands in `docs/TESTING.md` §3.

## 1. Header — 24 bytes

| offset | size | field | evidence |
|-|-|-|-|
| 0 | 4 | `PCD9` | the engine compares this dword with `0x39444350` and fatal-errors "Corrupt texture data passed into…" otherwise [V-exe] |
| 4 | 4 | format tag | `'DXT1'`, `'DXT5'` or `0x00000015`; nothing else in these 71 sections (25 / 14 / 32) [V-data] |
| 8 | 4 | payload bytes | equals the section size minus 24, for all 71 [V-data] |
| 12 | 4 | zero | always zero here; the engine never reads it [V-data] |
| 16 | 2 | width | `FUN_0060c430` passes the dword at +0x10 to the texture factory as a short [V-exe] |
| 18 | 2 | height | read at byte +0x12 in the same call [V-exe] |
| 20 | 1 | bits per pixel | `32` for tag `0x15`, zero for DXT1/DXT5 [V-data] |
| 21 | 1 | mip levels − 1 | the engine computes the level count as this byte plus one (`+0x15`) [V-exe] |
| 22 | 2 | usage | `3` on colour maps, `0` on masks in this unit; semantics otherwise unproven [V-data] |

## 2. Payload — the whole mip chain, largest level first

Per level *k*: extent `max(1, width >> k)` by `max(1, height >> k)`. A block format costs
`ceil(dim/4)` blocks per axis with a floor of one block, at 8 bytes (DXT1) or 16 bytes (DXT5); the
uncompressed tag costs 4 bytes per pixel. Summing those levels reproduces `payloadBytes` exactly for all
71 sections [V-data]:

| section | tag | extent | levels | payload bytes | chain check |
|-|-|-|-|-|-|
| 0 | DXT1 | 512×512 | 10 | 174 776 | 131 072 + 32 768 + 8 192 + 2 048 + 512 + 128 + 32 + 8 + 8 + 8 |
| 7 | DXT5 | 256×256 | 9 | 87 408 | 65 536 + 16 384 + 4 096 + 1 024 + 256 + 64 + 16 + 16 + 16 |
| 9 | `0x15` | 128×128 | 8 | 87 380 | (16 384 + 4 096 + 1 024 + 256 + 64 + 16 + 4 + 1) × 4 |

The two smallest levels of a 512-wide chain each cost a single block: that is why ten levels fit into
174 776 bytes and nine would not. A level count that disagrees with the payload length therefore puts every
block after the first at the wrong offset, so `parsePcd9` refuses it instead of uploading garbage.

## 3. What each tag holds

* **DXT1** — BC1 blocks, row-major, `(width + 3) / 4` blocks per row. Decoded to PNG by
  `oa-asset texture --section 0`: section 0 is Lara's clothing atlas (blue top, gloves, belt buckle), so the
  header, the chain rule and the block stride are all confirmed visually [V-data].
* **DXT5** — BC3. Section 7 holds 5463 blocks, of which only five carry non-zero BC1 colour bytes while
  every one has a varying alpha table: it is an opacity mask, not a colour map [V-data]. Nothing here needs
  a CPU decode; the renderer uploads `VK_FORMAT_BC3_UNPACK_BLOCK` (or `_SRGB`) and lets the blend use alpha.
* **tag `0x15`** — uncompressed 32 bpp stored B8 G8 R8 A8, with RGB measured constant `0xFF` across sampled
  pixels while byte 3 varies smoothly: these are masks, and they carry their shape in alpha [V-data]. The
  ZenHAX observation that Anniversary payloads start `50 43 44 39 15 00 00 00` ("PCD" + version 21) is this
  tag value at +4, not a file version — the same unit also contains DXT1 and DXT5 tags [S][V-data]
  (`docs/RESEARCH.md` §4).

## 4. Implementation

* `src/cdc/Texture.{hpp,cpp}` — `parsePcd9` validates the magic, a known tag, `payloadBytes` against what is
  actually present, `bitsPerPixel == 32` for the uncompressed tag, at most 32 levels, and a mip-chain total
  equal to `payloadBytes`. It returns false with a message instead of guessing. `pcd9MipChainBytes` is public
  because both validation and the eventual upload path need it. Pinned by `tests/test_texture.cpp`, whose
  golden numbers are sections 0, 7 and an uncompressed extent from this unit (no shipped data in the repo).
* `tools/oa-asset texture --path 'pc-w\lara.drm' [--section N] --out FILE.png` — decodes mip 0 for the eye:
  BC1/BC3 CPU decode plus a small zlib PNG writer. This is a verification aid only; the engine never runs it,
  since BC1/BC3 upload natively and uncompressed uploads as `R8G8B8A8` (alpha included, which is where these
  masks keep their detail).

## 5. Still open

* The usage word at +22 correlates with colour map (3) versus mask (0) in this unit alone; a wider sample of
  units is needed before treating it as anything but an opaque flag.
* Whether every `0x15` chain really runs largest-first: assumed from the two multi-level samples here, and
  enforced by the chain-length check rather than proven per level.
