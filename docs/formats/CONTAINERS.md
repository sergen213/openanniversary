# Container format: cdc `bigfile` archives in Tomb Raider: Anniversary (2007)

Evidence tags: **[V-exe]** observed in our own `tra.exe` disassembly, **[V-data]** reproduced against the
shipped install, **[S]** secondary source, **[I]** inference labelled as such.

Target binary: `tra.exe`, 32-bit PE, sha256 `b43ade0d922b0f007747e5707b97c86404cbf4f051829e3711f7007a656ea53a`
(GOG build). Install layout: `bigfile.000` … `bigfile.026`, 27 parts, payload total 4,079,466,079 bytes. **[V-data]**

All addresses below are virtual addresses in that binary. The decompiled sources stay in the sibling
`openanniversary-work/ghidra/` directory and are never committed.

## 1. Index layout **[V-exe]** `FUN_0047ddf0`

At byte offset 0 of `bigfile.000`:

```c
struct ArchiveFile {
    uint32_t numRecords;                 // 4304 in this install
    uint32_t hashes[numRecords];         // ascending, binary-searchable
    struct Record records[numRecords];   // parallel array, same order as hashes[]
};
struct Record {                          // 16 bytes
    uint32_t size;             // byte count after decompression
    uint32_t sector;           // absolute sector index across all parts
    uint32_t specMask;         // specialisation mask, see §4
    int32_t compressedLength;  // 0 = stored uncompressed
};
```

Identical to the layout published by [cdcengine.re](https://cdcengine.re/docs/files/bigfile/) for
Legend/Anniversary **[S]**; we derived it from `FUN_0047ddf0` and the shipped bytes independently.

Checks that hold on this install **[V-data]**: 4304 records, hash table sorted ascending (our loader refuses
an unsorted table because it would mean a wrong parse), 2967 distinct hashes, sizes 43 … 37,233,696 bytes,
`compressedLength == 0` for every record, and no record straddles a part boundary.

## 2. Path hashing **[V-exe]** `FUN_0047da10` → `FUN_0047d970`

`FUN_0047da10` runs `_strlwr` (ASCII-only folding) over the whole path, then `FUN_0047d970`:

```c
uint32_t hash = 0xFFFFFFFF;
for (unsigned char c : lowered_path) {
    hash ^= (uint32_t)c << 24;
    for (int i = 0; i < 8; ++i)
        hash = (hash & 0x80000000u) ? (hash << 1) ^ 0x04C11DB7u : hash << 1;
}
return ~hash;
```

Non-reflected CRC-32 with polynomial `0x04C11DB7`, init and final complement both `0xFFFFFFFF` — i.e. the
CRC-32/BZIP2 variant applied to the lowercased path string, extension included. cdcengine.re states only that
"since Defiance" the CRC-32/`0x4C11DB7` family is used; the folding and init/final parameters come from our own
disassembly **[V-exe]**.

Golden values **[V-data]** (also present in this archive):

| path | hash |
| --- | --- |
| `pc-w\generalbank.drm` | `0xB3AAAFD6` |
| `pc-w\lara.drm` | `0x5FF91900` |
| `\trae\pc-w\objectlist.txt` | `0xDD7335E5` |
| `\trae\game\pc\trae.arg` | `0x6854ECAC` |
| `\trae\pc-w\objlist.dat` | `0x77D15B59` |

The first two are byte-identical to the hashes recorded for the *Underworld* archive in the sister project
([openunderworld `docs/formats/CONTAINERS.md`](../../../../1st4TB/Projects/openunderworld/docs/formats/CONTAINERS.md)),
so the unit path convention `pc-w\<name>.drm` is shared across the Legend-era family.

## 3. Sector and part addressing **[V-exe]** `FUN_0047dbd0`

```
absolute = sector * 0x800          // 2048-byte CD-ROM sectors
part     = absolute / 0x9600000    // Legend/Anniversary split alignment
name     = sprintf("bigfile.%03d", part)
```

The archive is opened through the string `BIGFILE.DAT`; the `.000` suffix and part count come from the same
routine. cdcengine.re lists the same `0x9600000` alignment for Legend/Anniversary (Underworld uses
`0x7FF00000`) **[S]**. Verified: all 4304 records resolve inside a part under this mapping, 27 parts on disk.

## 4. Specialisation masks **[V-exe]** `FUN_0047da60`

Lookup is a binary search over `hashes[]`; on a hash match the record is accepted when
`(record.specMask & queryMask) == queryMask`, i.e. the stored mask must be a superset of the queried bits. The
shipped code checks the landed slot first, then walks the equal-hash window backwards and forwards; `oa-asset`
reproduces that order (the choice within a matching window is not observable from outside).

Observed masks in this install **[V-data]**:

| specMask | records | reading |
| --- | --- | --- |
| `0xFFFFFFFF` | 2634 | valid for every configuration |
| `0xFFFFFFE1`, `0xFFFFF808`, `0xFFFFF802`, `0xFFFFF810`, `0xFFFFF804` | 334, 333, 333, 333, 333 | five mutually exclusive variants of the same set **[I]** language-specific sound/media records; not yet confirmed by content |
| `0xFFFFFFE2`, `0xFFFFFFE8`, `0xFFFFFFE4`, `0xFFFFFFF0` | 1 each | single language-specific overrides |

## 5. Compression

All PC records in this install are stored (`compressedLength == 0`) **[V-data]**. The loader still implements the
zlib path — with a raw-deflate fallback — because Legend's Xbox 360 build and other cdc ports store compressed
records **[S]** (cdcengine.re: Zlib, `compressedLength > 0`).

## 6. Name resolution

Hashes are one-way, so names must be recovered elsewhere. Two sources worked on this install **[V-data]**:

1. The archive's own object list at `\trae\pc-w\objectlist.txt` (hash `0xDD7335E5`): first line a count (`949`),
   then `id,name` rows (`1,generalbank`, `3,lara`, …) — the same shape as Underworld's object list.
2. Path templates in `tra.exe`: `%s\generalbank.drm` with `%s` lowercased to `pc-w`; the hashed form is therefore
   `pc-w\<name>.drm` (no `\trae\` prefix), while text files use absolute forms such as `\trae\pc-w\objectlist.txt`.

Result: 948 of the 949 object-list units resolve, reproducible with

```sh
oa-asset "<install>" resolve candidate-paths.txt   # matched 948 of 949 candidate paths
```

### 6.1 Level naming and the chapter table **[V-data]** (2026-10-23)

The level list is not a loose file — it is `pc-w\generalbank.drm` (`0xB3AAAFD6`), section-0 image, 400-byte-stride rows
starting at image offset 4,388: `+0x00` a key (`chapter0`…`chapter4`, then `level1`…`level4` per chapter), `+0x20` a
level code. Chapter starts: `chapter0→ma1`, `chapter1→cn2`, `chapter2→cn4`, `chapter3→cn9`, `chapter4→cn8`; the
`levelN` rows give each chapter's ordered openings `(pu1,pu8,pu11,pu16)`, `(gr1,gr31,gr18,gr27)`, `(eg1,…)`, `(lc1,…)`
and a zone table further on (offsets ~17,440–20,896) lists every area code: `pu1…pu23`, `gr1…gr32`, `eg1…eg32`,
`lc1…lc20`, `cn1…cn9`, `ma1`.

Those codes are the unit names: `pc-w\<code>.drm` hashes to the real level records — `pc-w\cn3.drm` = `0x8BAF540E`
(16,532,812 bytes, a unit the geometry path already decodes), `pc-w\ma1.drm` = `0x5526C48B`, etc. Reproduce with
`oa-asset "<install>" resolve <codes-as-candidates>` (406 candidates, all large `.drm` units matched).

The code path agrees **[V-exe]**: `tra.exe` reads the arg record `\TRAE\game\pc\TRAE.arg` (43 bytes, shipped content
`pu 1 -mainmenu -srconfig="PC-W" -debugmenus`) and parses `-chapter N` / `-chaptervars N` into globals
(`DAT_00838864/68`), which call `FUN_004227e0(chapter-1, 0)`; that routine indexes
`chapter*400 + *(obj+0x1c)` — the object comes from the generalbank unit (`DAT_00812c68 = *(iVar1+8)+0x44` in
`FUN_0045f6e0`) — copies the row names to `DAT_008383fc`/`DAT_00895404+0x10` (current/next area), and level data
files are opened through the `%s%s.raw` / `%s%s.mul` name templates (`DAT_0086330c` prefix + area name).
The `levelN` rows themselves use a 212-byte stride; only their two string fields are used here — the rest of that
row layout stays open.

## 7. Payload identity after extraction **[V-data]**

```sh
oa-asset "<install>" extract --path 'pc-w\generalbank.drm' --out /tmp/generalbank.drm   # 90,384 bytes
oa-asset "<install>" extract --path 'pc-w\lara.drm' --out /tmp/lara.drm                 # 7,630,042 bytes
```

Both extracted units begin with `u32 14`, i.e. **DRM version 14**, the Legend/Anniversary unit format; Yura's
`DrmFile` accepts only version 14 for TR7/TRAE **[S]** and the sister project documents Underworld's version 19
(`hdr[0] == 0x13`) **[S]**. The unit header body is decoded in `docs/formats/DRM.md` §§1-3 and implemented in
`src/cdc/Drm.cpp`.

## 8. What the payloads are, counted across every record **[V-data]**

`oa-asset drm` was run over all 4,304 records of this install (`list` output fed to `--hash`/`--mask` one record at a
time). Grouping by verdict and leading dword:

| Leading u32 | Records | Reading |
| --- | --- | --- |
| `14` | **1,098** | DRM v14 units. All 1,098 parse byte-exactly with every texture magic verified (`docs/formats/DRM.md` §5): 1,097 closed on plain closure or the absent-entry rule, and record `F2197DE6` needed both rules of §3 |
| `44100`, plus `44099` (34), `44098` (5) and one each of `44097/44096/44094/44093` | 2,724 | audio streams: a sample rate in Hz at the payload start **[I]** — every value sits within a few units of 44.1 kHz |
| `36000`, `32000`, `48000`, `22050` | 42, 32, 13, 7 | more audio streams at those rates **[I]** |
| `0x52415721` = `"!WAR"` | 232 | an ASCII tag; role not yet established |
| `0x2173754D` = `"Mus!"` | 73 | music-related, judged by its tag **[I]** |
| `0` | 74 | payload begins with a zero dword; unclassified |
| text-shaped dwords: `0x0D393439` = `"949\r"`, `0x0D333531` = `"153\r"`, `0x31207570` = `"pu 1"` | 1, 1, 1 | plain text. The `"949\r"` record is `\trae\pc-w\objectlist.txt` (hash `DD7335E5`), whose first line is that count — checked by extracting it (§6) |
| unique one-offs (`1`, `2`, `3`, `4`, `31`, `154`, …) | 14 records, each a distinct leading dword | unclassified |

Reproduce:

```sh
oa-asset "<install>" list | awk '{gsub(/specMask=/,"",$5); print $1,$5}' > recs.txt
while read -r h m; do
    oa-asset "<install>" drm --hash "$h" --mask "$m" >/dev/null 2>/tmp/e.txt \
        || grep -o "unsupported DRM version [0-9]*" /tmp/e.txt | head -1
done < recs.txt | sort | uniq -c | sort -rn
```

Consequence for the reconstruction: the container path is no longer an unknown. Every v14 unit in the install can be
walked to its payloads; a general section's unindexed triangle run now decodes and draws (`docs/formats/GEOMETRY.md` §2,
§6), while what still has no reader is the index/oct-tree layer behind it (§5), the material-to-texture link, and the
audio/video payload families.
