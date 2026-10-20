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

The remaining ~3,350 records (textures, sound, movies, text) still need name candidates; that is the open item
tracked in `docs/ROADMAP.md`.

## 7. Payload identity after extraction **[V-data]**

```sh
oa-asset "<install>" extract --path 'pc-w\generalbank.drm' --out /tmp/generalbank.drm   # 90,384 bytes
oa-asset "<install>" extract --path 'pc-w\lara.drm' --out /tmp/lara.drm                 # 7,630,042 bytes
```

Both extracted units begin with `u32 14`, i.e. **DRM version 14**, the Legend/Anniversary unit format; Yura's
`DrmFile` accepts only version 14 for TR7/TRAE **[S]** and the sister project documents Underworld's version 19
(`hdr[0] == 0x13`) **[S]**. The unit header body (section table with `0xFFFFFFFF` terminators and an incrementing
index) is not yet decoded; see `docs/formats/UNITS.md` when it is.
