# Geometry inside DRM units (Anniversary)

What the container path hands over, and what has actually been measured about model and level geometry. Tags as in
`docs/formats/DRM.md`: **[V-data]** reproduced byte-for-byte from extracted files of the shipped install, **[V-exe]**
read out of `tra.exe`'s decompiled loader (`openanniversary-work/ghidra/exports/decomp_all.c`), **[S]** secondary
source, **[I]** inference.

## 1. Where the bytes are — section census [V-data]

Section payloads sit back to back after the base solved in `DRM.md` §3, largest first. Two units show that geometry is
not one uniform thing:

| Unit | type 0 (general) | type 2 | type 5 (texture) | type 6 | type 7 | type 10 | biggest single payload |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `pc-w\lara.drm` (7,630,042 B, 1712 sections) | 110 / 590,312 B | 450 / 2,306,716 B | 71 / 2,084,336 B | 229 / 2,176,320 B | 851 / 398,382 B (mostly 48/16/8/4-byte records) | — | type 5, 174,800 B |
| `pc-w\hash 8baf540e` level unit (16,532,812 B, 235 sections) | 117 / 4,159,040 B | — | 92 / 10,788,488 B | — | 6 / 22,776 B | 20 / 1,457,204 B (largest 489,252 B) | **type 0, section 0: 2,981,540 B** |

So a level unit keeps its vertex data in one huge type-0 payload, while `lara.drm` spreads its payloads over many
small type-2/6/7 sections and has no comparable array. "The largest type-0 section" is therefore *not* a reliable way
to find geometry [V-data].

## 2. A stride-20 vertex record exists in level units [V-data]

Level unit `8baf540e`, section 0 (`type 0`, payload offset 35,600, size 2,981,540 = exactly 149,077 × 20 bytes). Every
record reads the same way:

| Offset | Type | Field | Measured over all 149,077 records |
| --- | --- | --- | --- |
| +0 | i16 ×3 | x, y, z | −27,551…27,552 / −27,612…27,613 / −24,800…25,566 — one model-space bbox, plausible for a level chunk |
| +6 | u16 | unused here | `1` in 149,077/149,077 records (constant) |
| +8 | u32 | colour ABGR | byte 3 = `0x80` in 120,956, `0x66` in 19,788, `0x00` in 4,375; RGB varies per record |
| +12 | i16 ×2 | u, v | full −32,768…32,766 range; the community level viewer treats UV as fixed-point with scale 1/4096 [S] |
| +16 | u32 | unused here | `0` in 149,077/149,077 records (constant) |

Reproduce:

```sh
oa-asset "<install>" drm --hash 8baf540e                    # section table incl. the ABSENT tail entry
oa-asset "<install>" drm --hash 8baf540e --section 0 --out /tmp/sec0.bin   # 2,981,540 bytes
python3 - <<'EOF'
import struct
d = open('/tmp/sec0.bin','rb').read()
recs = len(d)//20
print(len(d), recs)
one = {struct.unpack_from('<H', d, 20*k+6)[0] for k in range(recs)}
tail = {struct.unpack_from('<I', d, 20*k+16)[0] for k in range(recs)}
alpha = {}
for k in range(recs):
    a = (struct.unpack_from('<I', d, 20*k+8)[0] >> 24) & 0xFF
    alpha[a] = alpha.get(a, 0) + 1
print(one, tail, sorted(alpha.items(), key=lambda kv: -kv[1])[:3])
EOF
# {1} {0} [(128, 120956), (102, 19788), (0, 4375)]
```

## 3. The same stride does not fit other big payloads [V-data]

`lara.drm` section 4 (`type 0`, 171,108 B) read at the same 20-byte stride gives a field that is *not* constant at +6
(values 5, 0, 100 …), and its +16 dwords include `0x3F800000` (= 1.0f) among other float bit patterns — so that payload
holds float-bearing structures (transforms or scene data [I]), not vertex records. Any geometry reader has to know
which section it is looking at, and the parser currently exposes payloads without claiming a record layout.

## 4. Relocations bind resources across sections; they are not primitive lists [V-data]

Per `DRM.md` §2.1 each entry names a **target section index** plus a slot in the current payload, kind 0 patching that
slot to `base(target) + addend`. In `8baf540e` all 8,874 entries are kind 0 and point at other sections — busiest
targets: section 78 (849 references), 87 (623), 132 (226), 141 (167), 142 (164). The vertex array of §2 is itself the
target of only 13 entries, and 11 of those addends are multiples of 20.

That rules out the cheap model in which a general section stores triangles as arrays of vertex pointers: if it did, the
vertex section would be referenced hundreds of thousands of times. Triangle indices therefore live inside the model's
own structures [I]. `lara.drm` section 4's 57 entries point into sections 5 and 29 with addends that chain forward in
small steps (slot `0x3964` → addend `0x3980`, etc.), i.e. linked tables rather than vertex arrays [I].

## 5. Open: the index and oct-tree layout — this is what blocks Milestone 4

Unknown, stated plainly: how a model's triangles reference the records of §2 (triangle list vs strip vs cache), where
the oct-tree nodes sit relative to the vertex array, how materials pick textures (candidate: the cross-section kind-0
references above plus the `id` field of type-5 sections), and what the 20 type-10 payloads of a level unit hold.

Next evidence, in order of directness:

1. ~~Decompile the per-type handler objects reached from the loader's handler table~~ - resolved, and it was a dead end for
   geometry: the table is `DAT_0088e2b0 + type*4`, registration happens in `FUN_005bd980` (slot 5 = `FUN_00403b30`, the
   texture handler of §TEXTURES; slot 7 = `FUN_005bc830`, the shared-instance cache of §7) [V-exe]. The type-2 handler's
   methods (vtable at file offset for `0x657474`, disassembled straight out of `tra.exe` around `0x5bc580`-`0x5bc660`) are
   an acquire/release pair, one memcpy patcher that writes into a block plus an offset, and accessors over a static keyed
   table at `0x84d41c` whose records hold a u16 field at +0 and a pointer at +4 - generic resource plumbing with no vertex
   walking in it [V-exe]. The level layout instead came out of the payload data: see §9.
2. ~~Cross-check against the community level viewer's claims (0xB0-byte oct-tree nodes, `vbBaseOffset`/`polygonStart`/
   `polygonCount`, model header `_octTreeCount` at +0x14, `_vertexStart` at +0x44)~~ - those numbers are for *Legend*. The
   Anniversary runtime decompilation now supplies the real shapes: see §8. Blind fitting of the Legend offsets over
   `8baf540e` produced only sliding-window artefacts [V-data].

Acceptance rule for any layout claim here: counts plus bounding box reproduced from a shipped unit **and** the mesh
rendered on screen. A plausible field table is not verification — §3 is what guessing looked like.

## 6. Drawing a triangle run in `oa-game` [V-gpu]

The decoder reads one general section as an unindexed run: whole 20-byte records, three consecutive records per
triangle, no index buffer. That is the shape §2 measured for level unit `8baf540e` section 0 (2,981,540 bytes = exactly
149,077 records), and it goes to the GPU as those same bytes — the vertex buffer *is* the payload, with position read
as `R16G16B16_SINT` at +0 and colour as `R8G8B8A8_UNORM` at +8.

    oa-asset "<install>" drm --hash 8baf540e --section 0 --out /tmp/sec0.bin      # the bytes, from docs §2 above
    oa-game --gameDir="<install>" --mesh=8baf540e --section 0 --selfCheck=true     # unnamed records take their path hash
    oa-game --gameDir="<install>" --mesh=8baf540e --yaw=0.7 --pitch=0.4 --zoom=1   # default: largest stored type-0 section

Measured on `AMD Radeon RX 7900 XTX (RADV NAVI31)` with validation layers enabled, 2026-10-20 [V-gpu]:

    drawing section 0 of '8baf540e' (id 0, type 0): 149077 vertices, 49692 triangles, bounds 55103x55225x50366 world units
    mesh probe: 6768/9216 pixels covered by 12829 distinct vertex colours, 0 unexplained

The first line is the acceptance rule of §5 met in part: counts and bounding box come out of a shipped unit (the vertex
count is exactly payload size over 20 [V-data]), and the mesh is visibly there — a desktop capture of a 900x600 window on
this Wayland session shows a level-sized silhouette with facet colours and depth occlusion against the clear colour, drawn
by the same code path that presents frames. The second line is `VkRenderer::renderMeshOffscreen()` drawing that run into a
96x96 sRGB attachment, reading it back and requiring every lit pixel to equal one of the shipped colour bytes, within one
sRGB step: zero exceptions. That pass covers upload, vertex format, stride, matrix convention and the colour path, and it
is the byte-exact check — a desktop screenshot is not one here, because this session's compositor remaps window pixels (not
one of roughly 540k window pixels equalled a shipped byte exactly, while the silhouette was clearly present).

What is *not* claimed: winding order (nothing is culled — a wrong guess would delete half of every level instead of
exposing the order), how the modulate bytes combine with textures, or that section 0 is the whole level. The colour path
is deliberately flat-shaded (`flat` in `shaders/mesh.vert` and `mesh.frag`): no material found so far ties those bytes to
an interpolation rule (§5), and a flat colour makes every pixel of a triangle one shipped byte, which is what turns the
probe into an exact check rather than "close enough". Smooth vertex colouring waits for evidence that says so.

## 7. Type-7 sections are shared resource records, not geometry [V-data] [V-exe]

Running the `CONTAINERS.md` §8 scan loop but keeping each unit's type-7 directory rows gives one row per section
(columns: unit, section index, size, id):

```sh
oa-asset "<install>" list | awk '{gsub(/specMask=/,"",$5); print $1,$5}' > recs.txt
while read -r h m; do
    oa-asset "<install>" drm --hash "$h" --mask "$m" 2>/dev/null \
        | awk -v u="$h" '$2==7 {printf "%s\t%s\t%s\t%s\n", u, $1, $3, $4}'
done < recs.txt > t7.tsv          # 37,871 rows in this install
```

**37,871 type-7 sections across 1,086 of the install's 1,098 version-14 units**, 13,196,830 bytes total. Sizes span
4 to 368,400 B over 347 distinct values; 53.7% (20,351) are exactly 48 B, then 128×1,616, 16×1,524, 32×784, 4×754,
77×574, 164×547, 7,376×525. A unit holds 1-8 of them (mode 4). Only 94.2% of sizes are multiples of 4, so the class is
not a single record array.

The `id` column is a key, not a per-instance counter: **8,458 distinct ids**, each with exactly one payload size
archive-wide (id `0x6d1`: 128 B in 192 units; id `0xf0`: 7,376 B in 153), and inside a unit every type-7 id is
different. Payloads of one class are byte-identical between units - md5 over samples of the 94,260 / 7,376 / 77 / 48 B
classes collapses them into single groups **[V-data]**.

Two content shapes sit in that one type:

- name-bearing records - u32 fields followed by an ASCII resource path stored inside the payload; the 77-byte class ends
  with `Event_triggered\time_bend_03.`;
- float-bearing blobs (94,260 / 7,376 / 368,400 B) holding values such as `1.0f`, `-1.0f` and `5.0f`, but with no uniform
  record stride: over both tested blobs the gcd of word gaps between `0x3f800000` occurrences is 1, so they are not plain
  vertex or matrix arrays **[V-data]**.

Field table for the dominant 48 B class, twelve payloads with distinct ids: words 0, 3, 4, 8, 9 and 11 are zero or nearly
so; word 1 is the only per-id value (descending `0x300`, `0x2ff`, `0x2fe`, ...); words 2, 5 and 6 carry template
constants (`0x04007f64`, `0x3f800000`, `0x7f51f032`).

The executable explains the sharing. Slot 7 of the handler table (`DAT_0088e2cc`) is built by `FUN_005bc830`, which sets
vtable `PTR_FUN_00657338` and clears a static array of `0x8000` dwords at `DAT_0086e2a8`. Its methods use that array as a
keyed cache: the acquire path allocates `size + 0x10`, stores the block in slot `key`, writes a refcount u16 at +0 (the
release path decrements it and frees on zero), a second u16 field at +2 whose source the decompiler lost, and an in-use
byte at +4; lookup hands back `block + 0x10` only while that byte is clear. Type-7 payloads are therefore shared
refcounted instances, one per id - which accounts for both the fixed size per id and the identical bytes across units
**[V-exe]**.

Consequence for §5: the mesh drawn in §6 comes from a level unit's type-0 payload, so type 7 is not where model geometry
hides. Type 10 of a level unit (20 payloads, largest 489,252 B) is still uncharacterised.

## 8. cdcEngine's tr7 terrain model supplies the index encoding [S]

Consulted source: `openanniversary-work/priorart/cdcEngine` - TheIndra55/cdcEngine, GPL-3.0 decompilation of the
Anniversary runtime (its tree is rooted at `tr7`, the engine's own name for this game). It is licence-compatible with this
GPL-3.0-or-later tree; below are struct semantics and file:line pointers, not copied code. This supersedes the
Legend-based numbers in §5 item 2 **[S]**.

- `tr7/game/terrain.h:129 TerrainRenderVertex` - four i16 position components (`x, y, z, w`), one int colour, two i16
  texture coordinates and a u16 pair `bend`, `bendindex`: **exactly the stride-20 record of §2**, with the fourth short now
  named `w` and the trailing four bytes accounted for.
- `tr7/game/terrain.h:30 OctreeSphere` - bounding sphere (4 floats), a strip pointer, a child count and up to 8 child
  pointers. The draw path recurses while the child count is non-zero and draws one strip list at each leaf
  (`tr7/game/pc/D3D/d3dterrain.cpp:128`). A node is therefore 56 bytes of fields, not the 0xB0 that a Legend tool claims.
- `tr7/game/terrain.h:46 TerrainTextureStripInfo` - `vertexCount`, `vmoObjectIndex`, `texScrollOffset`, `lightFlags`,
  `stripFlags`, `matIdx`, two sort vertices, `sortPush`, a next-strip link, then `__int16 stripVertex[1]`: **the index list
  of one triangle strip into the terrain group's shared vertex buffer**. Strips are collected per material and drawn with
  those i16 indices plus the material's `vbBaseOffset` added to the base vertex index
  (`d3dterrain.cpp:157-158`, `d3dterrain.cpp:244`) **[S]**.
- `tr7/game/terrain.h:67 XboxPcMaterialStripList` - `tpageid`, flags, `vbBaseOffset`, `numStrips`, strips pointer: a
  material names a *texture page id*, which is the material-to-texture link §5 was looking for. A terrain group holds a
  material list (`terrain.h:77`) plus a sorted-material index array (`terrain.h:107`).

Those are post-load structs whose pointers (`strips`, `nextTexture`, child arrays) must be filled in, which is what the
dense relocation entries of §4 are for: the file form should keep the same field order and let the loader patch the
pointers **[S → hypothesis, unverified]**. Consequence: strips and oct-tree nodes live in a level unit's *other* type-0
sections - section 0 is entirely stride-20 vertices (§2) - so the next check is to find, among `8baf540e`'s remaining
116 type-0 payloads, the blocks whose u16 values stay inside the 0-149,077 vertex range and whose counts match strip and
material records.

## 9. The level oct-tree and its leaf references, measured [V-data]

Section 86 of unit `8baf540e` (22,288 bytes) is the terrain group's oct-tree in file form. A node is:

| offset | field |
| --- | --- |
| +0 | four floats - bounding sphere centre and radius |
| +16 | u32 reference, used only by leaves (branches hold 0 here) |
| +20 | s32 child count (observed 0-8) |
| +24 | that many u32 **self-relative byte offsets** to child nodes, in ascending order |

Walking from offset 0 visits **623 nodes, 533 leaves, depth at most 6, with no anomalies**: every child offset
stays inside the payload and points past its parent's field list. Leaves are packed at the end of the payload at stride
32; a full eight-child branch occupies 64 bytes **[V-data]**. Reproduce:

```sh
oa-asset "<install>" drm --hash 8BAF540E --mask FFFFFFFF --section 56 --out /tmp/oct.bin   # 0x56 = 86
oa-asset "<install>" drm --hash 8BAF540E --mask FFFFFFFF --section 56                      # same call, no --out file:
                                                                                          # stderr lists its relocations
```

Every leaf's +16 slot is a relocation entry - **533 leaves, 533 sites** - and the references land in 34 sibling
sections; each of those sections is referenced by exactly 16 leaves, whose addends start at 0 and ascend, so a leaf's
block runs from its own addend to the next one **[V-data]**.

Those blocks were first read as **i16 triangle-strip index lists into section 0's stride-20 vertex array**: 496 of 530
blocks contain only values below 149,077 (the vertex count established in §2) with more than half non-zero, and 529 of
530 satisfy the strip property that every consecutive triple shares an index with its predecessor **[V-data]**. That
reading has now been measured against geometry and it fails: drawing each block as a triangle list indexed into section
0 gives an RMS longest edge of **20-64%** of the array's span for all nine trees, where genuine strips sit near 0.4%
(§10). So what a leaf's +16 reference addresses is open again. What was already established stands: §8's expectation of
one strip-bearing payload is wrong - the geometry-carrying sections are many small ones, reached through relocations
**[V-data]**. `oa-asset <game-dir> terrain --hash 8BAF540E` prints both figures per tree.

Every one of section 86's 1,155 relocations lands on a node field: **533 at the +16 slot of leaves** (the leaf
references above) and **622 at child slots**. Both groups are kind 0 with `typeSpecific` 0. The leaf group points at real
sibling sections; the child group is labelled target-section 87, whose payload opens with a `PCD9`/`DXT5` header - but the
stored values cannot be offsets into that payload: the largest is `0x56F0` (22,256) against a section-87 payload of
21,896 bytes, while every one of them lands inside section 86 and validates there as a self-relative child offset. So for
these entries our decoder's "section index" field is not the pointer target; whether it names an owning object (a texture
page id, matching `XboxPcMaterialStripList.tpageid` in spirit) or the entry encoding differs from `DRM.md` §2.1 for this
kind of slot is unresolved **[V-data]**. Consequence for rendering: file-form children are self-relative and can be walked
as measured, but neither the leaf-to-record link (§10) nor per-leaf material selection is known, so nothing indexed draws
yet - a level unit's array renders as one unindexed run (§6).

Note on `--section`: the directory prints section indices in **decimal** while `--section` parses **hexadecimal**, so
section 86 is fetched with `--section 56`. The relocation list of a section only appears when that section is selected.

## 10. What those payloads do hold: strip records, measured [V-data]

Prior art `tr7/game/terrain.h:46` gives `TerrainTextureStripInfo` as a 44-byte header - vertexCount i32 at +0,
vmoObjectIndex i32 at +4, texScrollOffset f32 at +8, lightFlags u32 at +12, stripFlags i32 at +16, matIdx i32 at +20,
sortVertexMaterial 3xi16 at +24, sortVertexSpectral 3xi16 at +30, sortPush i32 at +36 and nextTexture u32 at +40 - and the
file form matches it: that header, then `vertexCount` little-endian u16 indices into section 0's stride-20 array, with
the indices starting at +44 **[V-data]**.

Unit 8baf540e, measured by `oa-asset <game-dir> terrain --hash 8BAF540E`, which accepts a record start only when the
header is sane, its index run stays inside that array and the triangles it draws are coherent against it (RMS longest
edge under 1% of the array's span), and claims the bytes an accepted record spans so starts cannot overlap:

    strip records: 101 payload(s) hold 4284 record(s), 2827 chain to another record start;
                   they reference 15724/149077 vertices of section 0
    leaf addends landing on a record start: 85 of 1620

Two things follow. `nextTexture` in file form is an absolute byte offset **inside the same payload**: 2,827 of the 4,284
records point at another accepted record start, and section 79 chains as 0x0 (vc=39, matIdx=7) -> 0x7C (vc=60, matIdx=8)
-> 0x120 **[V-data]**. The oct-tree leaves, by contrast, do not point at records: only **85 of 1,620** leaf addends
coincide with a record start, and in section 83 the block that addend 0 addresses opens
`0x00905A4D, 3, 4, 0xFFFF, 0xB8, 0, 0x40`, which is no strip header **[V-data]**. Leaf-to-record linkage and those other
payload kinds are unresolved; reading the executable's terrain loader is the way to settle them.

Coverage is open as well: these records index only 15,724 of 149,077 vertices, roughly a tenth of the array. Whatever
draws the rest is unidentified - `vmoObjectIndex` and prior art's portal-adjust vertex arrays are the candidates
**[INFERENCE]**.

A second probe of the same question, over section 86's tree alone: parsing a record header exactly at each leaf addend
(sane `vertexCount`, index run inside the payload, every index below the array size) passes for **50 of 533** leaves and
fails for 451 **[V-data]**. The passing ones chain normally - target 183 addend 0 holds vc=384 matIdx=75 next=0x32C, which
is another record start - so the form is right and only part of the tree's leaves reach it this way.

## 11. Which per-type handler owns a terrain payload, measured [V]

`FUN_005bd980` fills the handler table `DAT_0088e2b0 + type*4` (`GEOMETRY.md` §7 established the indexing): one heap cell
per slot holding an object pointer - type 0, type 4 and type 10 all get `PTR_LAB_00657444`, type 1 `PTR_LAB_00657414`,
type 2 `PTR_LAB_00657474`, type 5 the texture handler returned by `FUN_00403b30`, type 6 `FUN_005cbb80`, type 7
`FUN_005bc830` (§7), type 9 `PTR_FUN_006574a4`; the two cells between them (`DAT_0088e2bc`, `DAT_0088e2d0`) are not set
there **[V]**. So the section types a level unit uses for vertices, oct-tree and strips are served by one shared generic
handler - there is no terrain-specific loader in that table, which is why those payloads look like plain byte arrays to our
parser. The generic path itself shows in `FUN_005bca80`'s default branch: a record count at object +0x14, an allocation of
`count * 0x14`, and a word-by-word copy over the payload in stride-20 steps **[V]**. Consequence for §10's open questions:
leaf-to-record linkage is decided by game code after loading, so the next place to look is the terrain-group constructor
among the callers of that generic handler, not the section dispatch itself.

## 12. Handler vtables read from `.rdata`, and what the stride-20 record really is, measured [V]

`tra.exe` ships only two PE sections (`.rsrc` covering `0x1000-0x85f000`, `.idata`), so every virtual address maps as
`file = 0x200 + (va - 0x401000)`; reading dwords there gives the handler objects §11 named. Each handler vtable is twelve
slots, tables laid out back to back: type 1 at `0x657414`, types 0/4/10 at `0x657444` (`FUN_005bc880`, `FUN_00498bc0`,
`FUN_005bc8d0`, `FUN_005bc8a0`, …), type 2 at `0x657474` (`FUN_005bc580`, `FUN_00498bc0`, `FUN_005bc5b0`, `FUN_005bc5c0`,
…), type 9 at `0x6574a4` (`FUN_005be350`, …) **[V]**. Most of those eight functions were not analysed by the earlier Ghidra
pass, so they are now dumped as per-function files in `openanniversary-work/ghidra/decomp` (the repo's
`tools/ghidra/scripts/DecompileFunctions.java` creates a function at an address when none exists).

What the type-0 and type-2 slots do contains no geometry interpretation: slot +0 is `FUN_005bc880`, which releases the
payload buffer through allocator vtable slot `+0x34`; slot +C is a plain word-at-a-time copy into the object buffer
(`FUN_005bc8a0`), and type 2's version copies into a static twelve-byte-per-entry table at `DAT_0084d41c[idx*3]`
(both `FUN_005bc5c0`) **[V]**. So the container layer never walks indices - it allocates, copies and frees bytes; which is
exactly why a level unit's payloads look like flat arrays to our parser.

The stride-20 walk in `FUN_005bca80`'s default branch (the object-instance loader: the version check quotes
`Wrong mkloadob version data %x, c=%x`) is therefore about *instance* records, and its own arithmetic names their fields: a
u32 key at +0, a resource-type byte at +4 that indexes `DAT_0088e2b0 + type*4`, a flag byte at +5 whose bit 0 marks an
already-merged record, a u16 argument at +6 handed to handler slot +0, a u32 grouping id at +C used by slot +4 to match
records that share one object, and a u32 mask at +0x10 tested against the unit's flag set **[V]**. Resolved handles are
written out as eight-byte entries: u32 handle plus u16 resource type (`iVar19 += 8`, `*(ushort *)(base + 4) = type`)
**[V]**. Those records are not terrain vertices - nothing in this path reads a u16 index array - so §10's strip indices stay
a game-code concern, read straight from the record body as prior art's `TerrainTextureStripInfo.stripVertex[1]` suggests
**[S]**.

## 13. Drawing the strip records indexed, and why §6's run is not the topology, measured [V-data] [V-gpu]

Header shape alone admits noise, so `decodeTerrainStripChain()` gates every candidate start with two tests taken from the
data rather than a constant: **index locality** (at least seven in ten consecutive pairs within eight places of each other -
a strip reuses its neighbours) and **edge scale** (the record's median longest triangle below four times the median longest
edge of the vertex array's own consecutive triples, sampled to 4,096 triangles). Census over unit `8baf540e`, same payload
extraction as §10 [V-data]:

    header-shaped starts             4774   in 101 payloads
    rejected by index locality        236
    rejected by edge scale            447
    accepted records                 4091   (chain links among them: 2638)
    array scale / gate limit       edge 172.7 -> reject above 690.6 world units

The accepted records draw 245,502 triangles [V-data]: longest edge median **198** world units, p90 536, p99 1,539, against a
level extent of 55,103 x 55,225 x 50,366. **79.8% of their edges are shared with another triangle** (one edge is reused by
45 triangles where strips meet). Their `matIdx` takes 92 distinct values, all in 0-91, none negative - material indices into
the unit's texture set, not garbage. The unindexed run reading of §6 shares **zero** edges: three consecutive records give
triangles of exactly the right size that are connected to nothing. Section 0 is a vertex array; these index lists are the
shipped topology [V-data].

One hazard measured on the GPU rather than in a notebook [V-gpu]: `0xFFFF` ends a strip inside an index run, and leaving
those words in the triangle list draws spikes across the whole level - with 149,077 vertices, index 65,535 is a valid vertex
sitting far away. The decoder keeps restarts as stored and drops any triangle straddling one; before that fix the same
payloads rendered as a starburst.

    oa-game --gameDir="<install>" --mesh=8baf540e --selfCheck=true --strips=true
    3882 strip record(s) in 2357 chain link(s): 262733 triangle(s) over 149077 vertex/vertices
    mesh probe: 4480/9216 pixels covered by 12829 distinct vertex colours, 0 unexplained

The binary's own counts sit a little off the python census above (3,882 records walked against 4,091 accepted starts); a
handful of borderline records fall on either side of the edge gate depending on how an even-length sample takes its median.
Both agree on what matters: nearly all header-shaped starts in terrain payloads are real strips.

Open after this: which texture a `matIdx` selects (92 values against the unit's type-5 sections), how oct-tree leaves choose
a sub-run inside these chains - §10 measured 15 of 16 addends landing *inside* an accepted record's index array rather than
on its header - and culling, since every chain in the unit draws whether the camera sees it or not.

## 14. What a walk over those strips exposed, measured [V-data]

§13's topology claim rests on edge sharing, which holds for most of the mesh; whether *every* accepted strip lands where it
should was untested until a point agent was walked over the decoded triangles (`TerrainQuery`, then `--walk=true`). It is not.
That probe has since been replaced by `--avatar=true` (§19), and these numbers were taken over the **ungated** strip list, so
they are history rather than something to reproduce:

    oa-game --gameDir="<install>" --mesh=8baf540e --strips=true --walk=true --frames=1   # flag removed; §19 is current
    4559 strip record(s) in 2804 chain link(s): 252,515 triangle(s) over 149,077 vertex/vertices, 25,832 sliver dropped
    terrain grid: 252,515 triangles, cell 201.4 units, 268x249 cells, y -7,755..27,613
    warn [app] the surface under foot comes from strip index lists whose vertex base is not established (GEOMETRY.md 14):
          these heights measure the decode, not shipped ground
    walk finished: 825 of 900 tick(s) supported, final (6,657.5, 25,422.0, 595.0)
    settled on triangle 247,354: (17,178 20,531 5,509) (4,649 26,352 5,509) (4,649 26,352 -3,032)

That last triangle is the falsification: an edge 13.8k world units long, surviving a gate set at four times its payload's own
median triangle edge. Per-payload gates are only as strict as the population they calibrate against, so a payload that is
mostly large triangles gets a loose limit [V-data]. Tightening the scale to the vertex array's consecutive-triple median
(§13's original baseline) drops 24,962 triangles but collapses the mesh it leaves: y falls to 1,978..16,380 and the z extent
to about 5.6k units of a 55k-wide level - over-pruning, because that baseline is not topology (§6).

A per-record census over the 117 cached payloads (header sanity: `vertexCount` 3-2000, `matIdx` -1..4096, `lightFlags`
<= 0xFFFF, index run inside the payload) separates real strips from header-shaped noise cleanly [V-data]:

    candidate records                     404
    index words below 149,077             61,658 of 61,658   (none out of range; only 1,344 read negative as i16)
    share of a record's triangles with an XY edge over 550 units: p25 0.000  median 0.000  p75 0.885
    records whose every triangle is short                       50.2%
    nextTexture (+40) naming the following record start         149 of 404;   zero (a chain tail) 17;   naming nothing 238

Bimodal, not a continuum: half the header-shaped starts are coherent surface, the other half draws nothing at this scale, and
a quarter are structurally linked to their neighbour. Linkage is therefore the stronger validator, and coherence a usable
second opinion - neither was what §13's gates used.

Why some strips land far away has one live explanation [S]: prior art keeps strip indices relative to a per-material vertex
buffer base (`tr7/game/terrain.h:67-74`, `XboxPcMaterialStripList.vbBaseOffset`), and §13 found records touching only 15,724
of the array's 149,077 vertices. No such table turned up in the cached payloads: scanning for `numMaterials` followed by
{tpageid, flags, vbBaseOffset, numStrips} at stride 16 or 20 with increasing bases, and for runs of eight or more increasing
multiples of 20, yields nothing [V-data]. Four offsets hold a lone word equal to 92 - the unit's material count - in sections
77 (0x6fd0), 82 (0x28), 141 (0x150) and 163 (0x28); what follows them is not that layout. Settling this means reading the
terrain-group constructor named at §11's caller list, where the game patches strip indices as it loads a unit.

## 15. Leaf references land on record headers once the relocation section numbering is fixed, measured [V-data]

§10's open question - only 85 of 1,620 leaf addends named a record start - was an off-by-one in our own parser, not in the
data. A relocation entry names its target section **one past** our directory index: the engine counts sections from 1
(`Drm.hpp`, read out of `tra.exe`'s unit loader `FUN_005bca80` case 5 [V-exe]). Subtracting one makes every self-referring
slot resolve inside its own payload - all 622 oct-tree child slots and the pointer slot at record +40 name own-index+1
[V-data] - and with that numbering every leaf reference of unit `8baf540e` lands on a record header whose index run stays
inside the target payload. Nine payloads hold oct-trees; their 1,620 leaves resolve into sibling type-0 sections:

    oa-asset <install> terrain --hash 8BAF540E        (the same numbers §16 reprints with link counts)
    section 77   nodes=800  leaves=657  depth=10 | heads=657  records=1913 badHeads=0
    section 86   nodes=623  leaves=533  depth=6  | heads=533  records=1262 badHeads=0
    sections 131, 140, 165, 190, 197, 200, 215: leaves 112, 83, 68, 79, 28, 52, 8; badHeads=0 for each
    vertices reached by these chains: 16338 of 149077 in section 0

So a leaf's +16 slot is the first strip record of that leaf's chain, exactly as prior art walks it (`DrawOctreeSphere` ->
`sphere->strip`, `tr7/game/pc/D3D/d3dterrain.cpp:182`) [V-data]. That closes §9 and §10. It also retires the payload-wide
header scan of §10-§14 from both library and tool: chains are entered only from leaf references now
(`decodeTerrainOctree` -> `decodeTerrainStripChains`). The scan was never a description of the shipped data - over unit
`8baf540e` it admits thousands of starts that sit inside another record's index run, and those are what §13's census and
§14's long joins were mixed with [V-data]. `TerrainStripChain` reports what a leaf-driven walk actually found: 4,776
records across the nine trees, all heads good.

## 16. Chains end on a zero-count marker, not on a guessed step, measured [V-data]

Following `nextTexture` (+40) as a raw in-payload offset ended every chain at its first link - broken links equalled the leaf
count for all nine trees. Two properties of the payloads explain that:

- The +40 slot is itself a relocation site. In twelve sampled strip payloads (sections 79, 82, 83, 84, 100, 105, 111, 126,
  132, 145, 160, 175) the section's own kind-0 entries sit exactly at header+40 and give 806 header positions; 731 of them
  pass §15's structural tests. The other 75 name index runs that reach outside section 0 - which is also what a base-offset
  scheme would produce, so they are not dismissed [S], see below.
- Padding puts the stored value short of the header it means. Records with an odd `vertexCount` pad their run to keep the next
  header on an even byte (section 79: a record with count 39 spans 128 bytes, six more than 44 + 2*39), and over those twelve
  payloads 580 of 750 layout-adjacent links name the following header exactly while 139 name a spot four bytes before it.

What ends a chain is prior art's own loop condition, `for (strip = tsi; strip->vertexCount > 0; ...)`
(`tr7/game/pc/D3D/d3dterrain.cpp:117`): the link is followed, and if the header reached has `vertexCount == 0` the chain is
over. Section 79 holds such a marker at offset 0x360; the record after it starts four bytes later, where that marker's own run
would have ended had its count been non-zero, so chains lie back to back and each one's last link names the next chain's
marker [V-data]. `decodeTerrainStripChains` follows links as stored and stops there - no forward-step guessing. Compared with
the earlier two-word step (1,511 steps taken on this unit): records, triangles and reached vertices are unchanged, and broken
links fall slightly (section 77: 42 to 41) [V-data]. So the steps never carried a chain into geometry that a leaf reference
did not already reach; what they did was walk through markers and merge neighbouring chains, which is why §16's own numbers
looked right while the walk was wrong [S].

    oa-asset <install> terrain --hash 8BAF540E
    section 77   nodes=800  leaves=657  depth=10 | heads=657  records=1913 badHeads=0 broken=41 | tris=43723 idx 0..16118 edge p50=63    p99=14460 max=86534 over2000=3.59%
    section 86   nodes=623  leaves=533  depth=6  | heads=533  records=1262 badHeads=0 broken=33 | tris=66453 idx 0..16337 edge p50=127   p99=51245 max=86534 over2000=4.52%
    section 131  nodes=141  leaves=112  depth=4  | heads=112  records=491  badHeads=0 broken=7  | tris=5857  idx 0..9304  edge p50=169   p99=56995 max=86534 over2000=6.50%
    section 140  nodes=98   leaves=83   depth=3  | heads=83   records=408  badHeads=0 broken=5  | tris=7538  idx 0..9542  edge p50=167   p99=56881 max=83400 over2000=5.73%
    section 165  nodes=80   leaves=68   depth=4  | heads=68   records=346  badHeads=0 broken=4  | tris=4806  idx 0..6536  edge p50=200   p99=58918 max=86534 over2000=7.86%
    section 190  nodes=91   leaves=79   depth=3  | heads=79   records=260  badHeads=0 broken=5  | tris=5117  idx 0..6177  edge p50=206   p99=60316 max=79573 over2000=8.53%
    section 197  nodes=35   leaves=28   depth=3  | heads=28   records=36   badHeads=0 broken=2  | tris=2182  idx 0..2253  edge p50=428   p99=63551 max=86534 over2000=29.77%
    section 200  nodes=69   leaves=52   depth=3  | heads=52   records=52   badHeads=0 broken=3  | tris=126   idx 0..233   edge p50=9926  p99=63698 max=63707 over2000=91.80%
    section 215  nodes=9    leaves=8     depth=1 | heads=8    records=8    badHeads=0 broken=0  | tris=112   idx 0..114   edge p50=6934  p99=59196 max=62096 over2000=88.10%
    vertices reached by these chains: 16338 of 149077 in section 0
Two things fall out of that table. What the chains index is **the contiguous front of section 0**: every tree's indices run
from 0 up to its own maximum, the largest maximum being 16,337, and exactly 16,338 distinct vertices are touched - so these
nine trees address one compact block at the start of the level array [V-data].

Second, edge length stays bimodal per record, not per decode: the six trees with the most chains have median longest edges
of 63-206 world units and 3.6-8.5% of edges over 2,000, while sections 200 and 215 - two to three triangles per record -
sit at medians 9,926 and 6,934 with ~90% over 2,000 [V-data]. Those are whole records whose every triangle is coarse, which
no linking rule can fix; a column query over the ungated list still answers with one of them (again an edge of ~13.8k units),
so the app keeps saying at Warn level that walked heights measure this decode rather than shipped ground - §19 gates the grid
a foot probes to what the app actually draws. §17 measures what does and does not
explain those records.

## 17. No per-material base is stored in the unit, and geometry alone cannot choose one, measured [V-data]

The coarse records of §16 look like an unapplied vertex base: prior art draws a strip with `vbBaseOffset + index`
(`tr7/game/pc/D3D/d3dterrain.cpp:244`, the field living in `XboxPcMaterialList` at `tr7/game/terrain.h:67-74`), and that list is
built at run time - no assignment to `vbBaseOffset` exists anywhere in cdcEngine. Three measurements on unit `8baf540e` say what
the shipped data does not give us.

1. **The bases are not in the file.** *(Wrong; §20 found them stored, and this item's search model is what missed them -
   kept because its negative results about geometry statistics still stand.)* For 79 per-(strip payload, material) offsets
   that make a group's triangles short (searched across all of section 0), scanning every stored payload for those values
   as u32 words finds them nowhere, and exactly one relocation entry in the whole unit names section 0: site s160+9360,
   kind 0, addend 0. Those offsets are computed by the loader, not stored beside the strips they apply to [V-data].
2. **Geometry criteria do not pick one out.** Ranking a (strip payload, material) group over every candidate base by *median
   longest edge* is worthless: thousands of bases score 0.0 because that metric rewards mapping indices onto repeated vertices -
   every sampled triangle degenerate. With an honest measure instead - the share of sampled triangles that are non-degenerate
   (shortest edge above 1 unit) with all edges under 1,000 units - 89 of the 287 groups holding at least 300 indices are already
   past 85% good at base 0, and for the rest hundreds to tens of thousands of bases tie within measurement noise. Section 215
   reads 0% good at base 0 and its best window reaches 97%, with 22,328 bases tied there; section 197 mat 0 goes from 4.7% to
   92.2% with 12,867 tied [V-data]. Choosing among those is guessing which lattice shift the game used, so no such rule belongs
   in the decoder.
3. **Blocks are not concatenated per material.** If each tree's materials held their own vertex blocks back to back - the shape
   prior art's bucketing suggests - one unknown start `S` per tree would fix every base at once, with
   base(material) = `S + sum of the strip vertex counts of lower-numbered materials`. Scanning all `S`: best medians stay 8-30
   units for trees 131/140/165/190/197/200, and the implied spans overlap between trees (tree 131 needs span 17,571 from
   S=16,923 while tree 140 needs 22,614 from S=17,178) - one array cannot hold both [V-data]. The model is wrong as stated.
4. **Base vertex indices exist per material at draw time, measured from inside the game's own process.** A d3d9
   passthrough shim standing in front of DXVK (`tools/dxhook`, setup in `docs/ENVIRONMENT.md`) logged every
   `DrawIndexedPrimitive` made while Lara stood in Croft Manor's entrance hall: 17,882,068 draw calls, 17,410,195 of
   them with at least 16 triangles, spread over ten distinct (vertex object, index object) pairs. Four of the ten ever
   use a non-zero base: the pair reached through stream offset 20 - vertex object `0x11fb92b8` with index objects
   `0x11fb8cb8`, `0x11fb8c38` and `0x11fb8d38` - is drawn with bases {0, 16097, 32447, 48540, 64754} across 10.0 M
   draws reaching at most vertex 72,832, while every pair reached through offset 24 is drawn with base 0 only and never
   passes vertex 21,845 [V-data]. The gaps between consecutive bases - 16,350 / 16,093 / 16,214 / 16,214 - are uneven,
   so each one marks where a material's vertex block ends and the next begins: that is prior art's
   `XboxPcMaterialStripList.vbBaseOffset` (`tr7/game/terrain.h:71`) arriving at `DrawIndexedPrimitive`, exactly as
   `tr7/game/pc/D3D/d3dterrain.cpp:244` draws with it. Item 1 still holds - the unit stores no such number - but their
   existence per draw is now measured rather than inferred, and what stays unknown is the ordering rule: which material's
   block starts at each base. Buffer bytes were never captured in that run - the `GetDesc`/`Lock` calls logged no successes,
   though the shim also carried a skip-list bug (`tools/dxhook`'s `already()` guard) that suppressed dumps on its own, so
   whether default-pool resources refuse `Lock` was never established and needs repeating before it is treated as fact.

So the decoder kept what it had at that moment - absolute indices, coherent for fine trees and badly wrong for coarse ones.
§20 has since found where those bases live in the file, so this section's remaining value is its negative results (geometry
statistics cannot choose a base) plus item 4, whose run-time numbers §20 now matches word for word out of the archive [V-data].

Three cheaper ways of reaching that code were tried and came up empty, so they are recorded here to stop repeats [V-data]:

- **No RTTI and no mangled names.** The 8,777,728 bytes of `tra.exe` contain zero occurrences of `.?AV`, `.?AU` or `??_7`.
  Class identification has to be structural; the repo's `ExportRtti.java` consequently writes an empty table.
- **String references are base-relative.** A new export, `tools/ghidra/scripts/ExportXrefs.java` (it reuses the cached
  project through `-process -noanalysis`, so a run costs seconds and one JVM), looked for every reference to the terrain
  diagnostic at `0x655d79`: no Ghidra reference, and not one stored little-endian dword naming that address anywhere in the
  image, aligned or unaligned. The console-command string at `0x650738` has exactly one - a table entry at `0x664cc4`.
  Where Ghidra did resolve a string it inlines it as an `s_..._<addr>` symbol, so grepping the decompile for those labels
  is the working route; grepping for the literal text is not.
- **Instance records are not kept as a stride-20 array on disk.** Laying §12's field table over every section of unit
  `8baf540e` whose size divides by 20 leaves no section where each record's +4 byte stays below 11, so the loader's
  in-memory form differs from the stored one.

The two functions that do name that diagnostic are `FUN_0050aa40` and `FUN_0050b840`; reading the second shows body and
navigation bookkeeping - a linked list of bodies, fixed 32-entry node arrays, per-body radius scaling - and no strip walk
[V]. Static hunting is capped there: cross-references in this image are base-relative, so `ExportXrefs.java` finds nothing
for most targets and the call graph cannot be rebuilt by grep. What changed today is that the shipped game now boots into a
real level on this machine under Wine plus DXVK (`docs/ENVIRONMENT.md`, "Running the original for observation"): the window
shows Lara standing in Croft Manor's entrance hall, screenshot verified [V-data]. That makes the decisive measurement a
memory question rather than a disassembly one - locate the loaded level payloads and any vertex buffer the engine built from
them, and diff those bytes against the archive's. If index words were rewritten on load, the diff shows it; if not, a base
value sits in some structure beside the oct-tree or its draw record. Direct `/proc/<pid>/mem` reads do not work: Yama is at
`ptrace_scope = 1`, and Wine parents guest processes under `wineserver` - even a wineserver we start ourselves leaves the
guest reparented to `systemd --user`, so no process we start ends up an ancestor of it, and the read returns `EACCES`
[V-data]. Wine's own debugger is not a way out either: this Wine (11.20) has **no `memsave` command at all**, that string
appears in none of its binaries, so the earlier pointer to it was never verified [V-data]. What does work is a small d3d9
passthrough DLL loaded inside the guest itself (`tools/dxhook/`): it takes DXVK's place as `syswow64/d3d9.dll`, keeps DXVK
reachable as `d3d9_dxvk.dll`, and rewrites the created device's vtable slots, which makes draw parameters and buffer
pointers readable from inside the process with no ptrace at all - that is how item 4 above was measured. A GPU capture would
answer the same question at the API level, but RenderDoc's Linux build injects nothing into Wine guests and produced no file
[V-data].

## 18. An edge-length gate removes the gross chains and still certifies nothing, measured [V-data] [V-gpu]

`--strips=true` draws the strip chains reached from every terrain tree's leaf references and gates what reaches the screen with
the rule §17 uses to score a base: non-degenerate (shortest edge over 1 unit) and longest edge under 1,000 world units. Two runs
over `8baf540e` section 0 measure how little that rule discriminates:

1. **Halving the threshold barely changes what is drawn.** At 2,000 units the gate stopped 13,130 triangles; at 1,000 units it
   stopped 16,462 of the 135,914 sampled - 119,452 drawn over 149,077 vertices, reached from 4,776 strip records through 1,620
   leaf references, with 0 bad heads and 100 broken links [V-data]. Only 3,332 more triangles fall out when the bar moves by a
   factor of two, so most chains that look wrong on screen have every edge comfortably inside the limit.
2. **Because section 0 is dense.** Its measured bounds are 3,282 x 2,447 x 1,640 world units holding 149,077 vertices, which puts
   the mean volume per vertex at 88 thousand cubic units and a nearest-neighbour scale near 45 units [V-data]. Pairing arbitrary
   nearby vertices therefore produces short edges by accident: short edges are necessary for a correct chain, never sufficient.
   That is also why §17's good-rate metric saturates - thousands of wrong bases score above 85 % - and why the gate belongs in
   presentation hygiene only. It is not evidence about which chains are right, and no ranking may be built on it.
3. **On screen the picture matches that reading.** With a perspective frustum (`--fov=55`) at fixed yaw/pitch/zoom, the window
   shows one large dark triangulated sheet, holes exactly where gated chains were dropped, and bright fans of near-degenerate
   triangles; nothing reads as a level [V-gpu]. Milestone 4's "recognizable scene" bar is not met by strip walking alone.

The conclusion at the time was §17's, and this section confirmed only that no statistic can replace it. What was missing has
since been found - the per-material bases are stored in the unit (§20), which is what turns these chains into ground a foot
can stand on; the gate itself stays presentation hygiene, not evidence [V-data].

## 19. A body moved over the gated strips stands only where those chains cover ground, measured [V-data] [V-gpu]

A foot now probes exactly what reaches the screen: `--avatar=true` builds its uniform grid from the triangles that survived
§18's edge gate instead of every chained triangle, because a column query answers with the *highest* covering triangle and the
ungated list carries slivers tens of thousands of units across. One run of unit `8baf540e` section 0, fixed script (walk east
2,000 units at 400 u/s, take one jump at a fixed tick, keep walking) using our own tuning, printed in the log:

    oa-game --gameDir="<install>" --mesh=8baf540e --strips=true --avatar=true --selfCheck=true --frames=1
    terrain grid: 119,452 triangle(s), cell 196.5 units, 159x29 cells, y 1,978..16,380
    avatar census: 106 of 2,304 sampled column(s) supported across 31,091x5,509 units; densest spot scores 8/8 at
        (-4,913.4, 9,497.9) ground 11,948.0
    avatar run over '8baf540e': tuning walk 400 run 750 gravity -2000 jump 620 step 25; 300 tick(s) ran, 229 supported,
        71 airborne, 1 jump(s), apex 92.2 above spawn ground 11,953.0, path length 2,000.0 units, 17 level change(s):
        still standing on the level

Four things come out of it:

1. **Coverage, not physics, is what limits a foot** [V-data]. Sampling 48x48 columns across the grid's own footprint finds
   support in 106 of them - 4.6% - which is §16's compact block showing through: nine trees addressing 16,338 of the section's
   149,077 vertices cannot tile a 31,091 x 5,509 unit footprint. The spawn search therefore scores every sample by how many of
   its eight neighbours one cell away are supported as well and takes the densest (8/8 here); from that spot 229 of 300 ticks
   land while walking straight east. The 71 airborne ticks are gaps at cell scale *inside* the patch - these chains do not
   cover their own footprint continuously - not a failure of the query.
2. **The jump arc matches our tuning, which is all it can match** [V-data]. `jumpSpeed` 620 against gravity -2000 gives an
   ideal apex of 96.1 units; sampling height once per 1/60 s tick reports 92.2 (96%), the difference being where sampled ticks
   fall around the top of the arc. The five `Avatar.*` tests hold the core to its tuning on synthetic floors, so this number is
   about coverage and the decode, not about whether the integration works.
3. **The old fixed-line probe died of this** [V-data]. §14's `--walk=true` started at the mesh centre and walked +x; over the
   gated list that start sits over a hole, so it fell through at tick 229 and exited with an error while reporting nothing the
   avatar run does not report better (support counts, level changes, a jump). It is gone rather than kept as a second probe.
4. **A keyboard-driven body walks, sidesteps and jumps on these triangles, and falls off the patch** [V-gpu].
   `--avatarDrive=true` hands the same body to W/S/A/D plus Space (left shift runs), with ticks taken from a wall-clock
   accumulator at fixed 1/60 s: presentation here runs ~158 fps, and one tick per presented frame measured 156 ticks/s - 2.6x
   faster than the tuning says. With pacing on, consecutive periodic reports land exactly 2.0 s apart for 120 ticks, a held
   sidestep moves the body (+305.9, +257.7) = 400 units along the camera's right basis over one report window at walk speed,
   and Space lifts it ~86 units above its ground line before contact returns. Walking east off the covered patch dropped it to
   y=1,773, where the app stops moving it with a Warn naming the hole. Offscreen dumps from her vantage (`--avatarFollow=true
   --meshDump=<path>`, kept as `openanniversary-work/wmem/follow.png`) show the triangulated patch and its holes rather than a
   level, exactly as §18 predicted for gated strips.

So Milestone 5's mechanics run over real decoded triangles today. What they could not do at the time of writing was stand on
shipped ground where the level actually is; §20 has since supplied what was missing - the per-material bases are stored in the
unit, and applying them reaches all 72,832 vertices of that array rather than only its first block.

## 20. The unit does store the per-material vertex bases, as a record array in one of its sections, measured [V-data]

§17 item 1 said these numbers were not in the file. It was wrong: they sit in a section of their own, laid out as records
of **stride 20** whose first word is the base, and reading them fixes the geometry §16-§19 could not explain.

Unit `4956BEA0` (the entrance hall §17 item 4 measured from inside the running game) holds such a section at index **43**:
a run of **137** records starting at payload offset 9,340, each record holding base u32 (+0), zero (+4), a slot that a
kind-0 relocation patches and that names this same section (+8), one packed word whose low half matches a texture-page
section id in 116 of the 137 records (§21; its high half is 32 in 114 of them but also 0/36/38/42/4256 - the earlier
"32 in every record" was a sampling artifact), then zero (+16). The base values along
the run are `{0 x25, 16097 x31, 32447 x29, 48540 x28, 64754 x24}` - the same five numbers §17 item 4 measured arriving at
`DrawIndexedPrimitive`, now read out of the archive [V-data].

The rule that ties them to strips is positional: **record i belongs to material index i**, and a strip record's `matIdx`
(header +20) selects it. Measured over all 2,339 chain records walked from unit `4956BEA0`'s one oct-tree (section 38):

- the chains name matIdx values **exactly 0..136 with no gaps**, matching a table of exactly that length;
- every record's largest index lands at or just under the capacity its base implies, and for four of the five blocks it
  hits the ceiling precisely - `16,096` against capacity `16,097`, `16,349`/`16,350`, `16,092`/`16,093`, `8,077`/`8,078`
  - so all 2,339 records fit with **zero** overflows [V-data];
- applying the bases turns indices absolute (`base + index`) and every vertex of section 0 becomes reachable: reached
  vertices go from `16,350 of 72,832` to `72,832 of 72,832`, median longest edge falls from 355 to **153** world units,
  and the share over 2,000 units drops from 8.53 % to **1.94 %**, with `unknown=0` (no record naming a material outside
  the table) [V-data].

So §17 item 1's scan missed them because it searched for per-(payload, material) *offsets* computed under its own wrong
model of what an unapplied base looked like; and §18/§19's conclusion - that `tra.exe`'s vertex-buffer build has to be
read before the strips can be trusted - no longer holds.

The library reads this table as `oa::cdc::decodeTerrainMaterialBases()` (`src/cdc/Geometry.*`) and applies it inside
`decodeTerrainStripChains()`, which now emits absolute indices; both callers (the app's strip path, `--strips=true` /
`--avatar=true`, and `oa-asset terrain`) ask for the table once per unit. The detector is deliberately narrow - kind-0
slots at a stride of 20 naming their own section, longest such run at least eight records, zero in word +4, bases
non-decreasing - and it refuses rather than guesses. A wrong pick cannot corrupt geometry silently: the capacity test then
drops those records into `badHeads`, or `unknownMaterials` when a chain names a material index beyond the table [V-data].

Sweep over every archive record between 6 MB and 25 MB (133 records, of which **57** parse as v14 units carrying at least
one oct-tree): **45 of those 57 yield a base table**, `unknown=0` in 43; two refuse whole groups of records against the
table they got (`6C476294`: 302 unknown, `CE53803B`: 69), and **12 units give no table at all** [V-data]. Units carry other
runs with the same stride-20 shape that are not terrain material tables - in `8baf540e` sections 153/160/188/191/206 hold
such runs whose first words (79,702 / 95,369 / 111,332 ...) belong to per-object model geometry rather than the level's
array - and today's rule simply takes the first section that passes, which lands on `8baf540e`'s real table (section
**141**, **92** bases over `{0, 16119, 32020}`, unknown=0) by ordering luck rather than by a rule [S].

Still open, in the order that matters:

- **Which section is *the* terrain table.** Resolved by the census rule (`findTerrainMaterialBases`, §21): walk every
  oct-tree base-free, take the first decoded table whose length covers every `matIdx` the walked records name. It picks
  `8baf540e`'s section 141 and `4956BEA0`'s section 43 by rule, not ordering luck [V-data].
- **What +8 and +12 mean.** Partly answered by §21: +12 is a (class << 16 | value) pair and class 32's value names the
  material's texture-page (type 5) section id for 160 of the 172 class-32 records across both probe units; the other
  classes' values name ids in a namespace that is neither the unit directory nor the global object list (§21 table).
  The +8 slots carry relocations naming their own section with addends (in this unit 12,072..21,384, monotone, ~50-80
  apart per record) that land in a stretch of that payload holding nothing but zeros, so they are not strip-record
  pointers [V-data]. Both stay open beyond what §21 measures.
- **The coarse remainder.** Even with bases applied some trees keep large shares over 2,000 units (sweep maxima run from
  3.5 % to 91.8 %, e.g. `8baf540e`'s ninth tree at 91.8 %) - the sections §16 already flagged as coarse-record trees are
  not explained by bases [V-data].

## 21. The census rule picks the table, and the material record's +12 low half names its texture page, measured [V-data]

Two results on the §20 table, both reproducible:

1. **Table selection by material census.** Walk every oct-tree of the unit once with no table (§15's base-free walk),
   collect the set of `matIdx` values (strip header +20) the reached records name, and take the first section whose
   decoded run is longer than every named `matIdx`. §20 flagged same-shaped runs serving per-object model arrays
   (`8baf540e` sections 153/160/188/191/206); none of them covers its unit's census, so the rule lands on section 141
   there and section 43 in `4956BEA0` without ordering luck. A unit where no run covers the census refuses and keeps
   §13's absolute reading. Implemented as `oa::cdc::findTerrainMaterialBases()`; both `oa-game --strips=true` and
   `oa-asset terrain` use it [V-data]. Reproduce: `./build/oa-asset <game-dir> terrain --hash 8BAF540E` prints
   `section 141: 92 material bases over 0 16119 32020`.
2. **+12 is a (class << 16 | value) pair; class 32's value is the material's texture-page section id.** Cross-unit
   per-high-half census over both tables, resolving low16 against the unit's own section directory [V-data]:

   | unit | high 32 -> type-5 id | high 32 unresolved | non-32 highs -> type-5 id | non-32 unresolved |
   | --- | --- | --- | --- | --- |
   | `4956BEA0` (137 recs) | 111 | 3 | 5 | 18 |
   | `8BAF540E` (92 recs) | 49 | 9 | 7 | 27 |

   So for the dominant class (high 32, 172 records over both units) the material's texture page is named in-file: 160 of
   172 lows are ids of the unit's own type-5 (texture) sections, and the handful of non-32 highs that resolve also name
   type-5 ids. The unresolved lows (9,466..10,540 plus 18,438/18,630/35,009) cluster above the type-5 id range of their
   own unit, share values across the two units' lists, and appear in no section directory and nowhere in the extracted
   global object list - which rules out "unit section id" and "global object-list id" for them and leaves them a
   second namespace (cross-unit or runtime texture handles). Distinguishing that needs one texture page actually
   sampled at draw time - which `matIdx` binds which `PCD9` - parked as Milestone 4's first experiment, not guessed
   at here [V-data]. And the pages the class-32 values name are real textures end to end: `oa-asset texture --hash
   4956BEA0 --section 1` decodes a 1024x1024 DXT1 chain with 11 mips, sections 2/3 512x512 with 10 [V-data].
   Reproduce: dump the table with `openanniversary-work/matrec <drm> <section>` (43 and 141) and
   the section directory with `./build/oa-asset <game-dir> drm --hash 4956BEA0` / `... 8BAF540E`.
3. **A luma-ranking probe cannot confirm the binding - recorded so nobody reruns it.** A statistical test over
   `4956BEA0`'s 108 class-32 materials with triangles (mean texel luma of DXT1 mip 0 at the flipped-V wrapped vertex
   UVs, named page ranked against the unit's other 35 DXT1 pages) came out indistinguishable from random: mean rank
   17.4 of 36, rank-1 in 2 materials, last in 4. Luma does not carry the binding signal - a bright page scores well
   for every material - so the class-32 reading stays supported by the id-space coincidence (§21 item 2), not by this
   test, and confirmation comes from drawing one material with its named page and comparing against the shipped game
   (Milestone 4's experiment a). Reproduce: `openanniversary-work/texbind /tmp/4956BEA0.drm 43` [V-data].

## 22. With bases applied, the entrance hall covers ground a foot stands on: 715 of 2,304 columns, measured [V-data]

§19's census over `8baf540e` under base 0 found support in 106 of 2,304 sampled columns. The same headless census over
the unit §20/§21 actually decode - `4956BEA0`, its section-43 table applied through §21's census rule - stands on
**715 of 2,304 columns (31 %)** across a 26,782 x 12,936-unit footprint, drawing 52,613 of 53,154 chained triangles
with only 541 gated by §18's edge rule (vs §18's 16,462-of-135,914 when bases were missing), and a body walking the
fixed script from the densest 8/8 spot keeps ground through consecutive ticks (`--mesh=4956BEA0 --strips=true
--avatar=true --selfCheck=true --frames=1`, 2026-10-21) [V-data]. The gate's remaining refusals and the 46 broken links
are §21's unresolved classes, not the base question; `y` still spans -17,656..17,731 because multi-storey level volume
is in one vertex array, which the oct-tree (not the grid) is expected to separate. This is §20's claim demonstrated on
feet rather than on edges: the bases produce continuous, walkable ground in the unit whose draw-time numbers they were
read from. The coarse-tree remainder of `8baf540e` (§20 open item 3) stays open and is the next geometry question.

## 23. Bind the class-32 page at draw time and this unit's materials redraw with their shipped surfaces: measured [V-data] [V-gpu]

This is §21's parked Milestone-4 experiment (a), run. The renderer now binds one texture page per material at draw
time: the triangle chains that survive §22's gate are grouped by `matIdx`, each material's `+12` class-32 value
resolves to that material's own `PCD9` page, uploaded as a `sampler2D` bound to that material's draw; the fragment
samples with the vertex UV (§17's quarter-unit lattice, wrapped addressing) and multiplies by the shipped flat vertex
colour. A material whose page does not resolve binds a white 1x1 instead, so its run stays byte-identical to the
§18/§22 colour-only picture.

1. **Resolution counts on `4956BEA0`, measured [V-data]:** 136 materials have drawn triangles; 133 of them name a
   page that resolves to one of the unit's 31 distinct type-5 sections; the three that do not all name id 9,722 —
   §21's unresolved cluster, which starts at 9,466. The values resolve in exactly one namespace: the **`id` field**
   of the unit's type-5 directory entries (§21 item 2), never the directory index — an index lookup resolves 0 of
   136, recorded so this dead end is not retried. Reproduce: `oa-game --gameDir="<install>" --mesh=4956BEA0
   --strips=true --textures=true --selfCheck=false --frames=1` logs `136 material(s) grouped into 136 batch(es)
   over 31 distinct PCD9 page(s) from named sections` plus one Warn per unresolved material (2026-10-22).
2. **What the picture says, compared against the shipped game at the same place [V-gpu]:** the dump
   (`--meshDump=<work-dir>/cap/oa_4956BEA0_tex.bmp`, the whole unit's bounded fragment drawn with its named pages)
   shows mossy stone blocks, ivy hanging in strands from wall tops, planted greenery, and cut-out foliage with the
   pages' own alpha shapes. The shipped capture of this hall (`<work-dir>/cap/level.png`, taken under Wine+DXVK from
   the install's own start room) shows the same surfaces on the same architecture: the identical block-moss pattern
   on the corridor walls, ivy strips hanging from the ceiling edges, and the same planting at the corridor end.
   Every drawn surface sits on the geometry the shipped game puts it on; no page lands on a surface it does not
   belong to. That settles §21's open question for the class-32 namespace: a `matIdx` selects the page its `+12`
   names, end to end. [V-gpu]
3. **What was tried and ruled out:** §21 item 3's luma ranking (no signal — recorded there, not rerun); the
   directory-index namespace (0 of 136, above). Two GPU notes kept so the path does not bite again: `VkDescriptorPoolSize`'s
   `descriptorCount` is the pool total rather than per set, and a pool sized exactly at the set boundary is refused
   (`VK_ERROR_OUT_OF_POOL_MEMORY`, `-1000069000`) — headroom over both.
4. **What stays open:** id 9,722's second namespace (three materials draw colour-only, §21 item (b)); per-face
   confirmation that one page's boundary ends at a surface edge rather than across a face (surfaces land on the
   right geometry at a glance; page-to-page seams are not yet checked); wrapped sampling hides a whole-page V-flip
   at this coverage, so the flip question stays with §17's. The colour-only self-check path is unchanged and still
   reports `0 unexplained` bytes.

## 24. The levels own jump envelope, measured across the sixteen largest v14 units [V-data]

The candidate scan of §19 doubled as a measurement when asked: `--gapEnvelope=true` records EVERY edge a
runway+shore line meets - not only jumpable ones - with the void's TRUE width (probed to 12 cells; beyond
reads as open water) and reports per unit: edge count, widths p50/p90/max over the voids that close inside
the cap, and how many of those one flat-ground jump at run speed could clear with the §19 tuning.
`openanniversary-work/run-gap-envelope.sh` runs it over `v14-top16.txt`, selected by rule: container records
whose specMask is FFFFFFFF, whose image parses as DRM `version=14`, and whose type-0 (general) section is at
least 800 kB - 66 units qualify, the sixteen largest of them by that section's size, the same set §4 of
docs/TESTING.md crossed holes on. The reach column varies per unit (344-446 units) because the census cell
comes from that mesh's own median edge; edge counts count candidate lines, so one small pond seen from
eight directions counts eight times.

| unit | edges | p50-p90-max closed voids | clearable | reach |
|---|---|---|---|---|
| 8baf540e (start hall) | 39 | 1562-1832-1851 | 1 | 426 |
| c2a23383 | 605 | 666-888-917 | 201 | 446 |
| 2388b2f0 | 3458 | 2322-2541-2596 | 383 | 410 |
| b1927dea | 1721 | 2161-2389-2415 | 297 | 414 |
| 2dae6592 | 644 | 3462-3695-3734 | 104 | 387 |
| b76fbce6 | 2053 | 2991-3308-3343 | 180 | 395 |
| 7bad4270 | 1929 | 3879-4049-4049 | 132 | 380 |
| 63926cc3 | 1527 | 2989-3230-3264 | 180 | 396 |
| others (8 units) | 37-981 | 062-5822 wide | 3-86 each | 344-422 |

What this bounds, honestly: (1) every unit shows designed short edges - the same kind §19 verified on the
start hall - that the tuned body clears; (2) the typical void on walked lines is 2,000-5,800 units wide at
every unit, ten times the model's reach: the model never claims those, and the levels connect such rooms
by their own routes, not by one jump; (3) the model's reach never exceeds even the SMALLEST closed-void
population median (c2a23383's 666), i.e. the tuning sits inside the levels' own envelope and cannot
over-reach what the designers left as their smallest deliberate gap at that unit. What it does not settle
is the engine's own jump numbers - that half of the M5 cross-check is the pending static read of tra.exe's
player-update constants in the Ghidra project [V-exe-still-open]; the numbers above are [V-data] evidence
about the LEVELS (designer-placed bounds), read through the §16-§19 decode, not about the engine's limits.

## 25. Splitting the walk per oct-tree changes which table a tree uses, and section 0 is every tree's array [V-data]

`oa::cdc::decodeTerrainUnit(drm, vertexArraySection, rule, ...)` walks EVERY oct-tree of a unit once and lets each tree pick its own base table from the same §20 candidate pool `findTerrainMaterialBases` uses; only the census scope changes (one tree's leaf groups instead of all trees'), so both rules print side by side with no second decoder in play. `oa-asset terrper --hash 8BAF540E --mask FFFFFFFF` shows first-covering (§21) and shortest-covering disagree on this unit - tree 86 moves to section 160, tree 131 to 188, ... - and the edge statistics move both ways: tree 215 goes from 88.1 % of edges over 2,000 units down to 0 %, while tree 131's share rises 6.5 -> 15.3 %. Neither census rule is a shipped answer on its own [V-data].

The other half of the split question - does a tree index some OTHER stride-20 section instead of section 0? - got a direct measurement: `openanniversary-work/arrayfit.cpp` re-runs `decodeTerrainUnit` against every stored section whose size is a whole number of 20-byte records (59 candidates on `8baf540e`, >= 256 records) and measures the chains' edges IN that candidate array. Every tree's chain counts stay identical across arrays - indices never leave range because the tables keep them inside - yet only section 0 produces plausible edges: for trees 131/165/190/197 every other candidate puts 67-100 % of their edges over 2,000 units. The vertex array is unit-wide; the open question was only ever about bases [V-data].

## 26. A tree's own strip payloads name its base table through kind-0 relocations [V-data]

`openanniversary-work/relotree.cpp` histograms, per oct-tree, every kind-0 relocation target reached from that tree's strip PAYLOAD sections and marks which targets pass §20's detector. On `8baf540e` each tree names at most one candidate table, and it differs between trees:

| tree section | payloads | candidate tables its payloads relocate |
|---|---|---|
| 77 | 42 | 141 (x160) |
| 86 | 34 | 160 (x76) |
| 131 | 8 | 206 (x16) |
| 140 | 6 | 153 (x60) |
| 165 | 5 | 191 (x49) |
| 190 | 6 | 188 (x46) |
| 197 / 200 / 215 | 3 / 4 / 1 | none - their payloads' kind-0 targets are not §20-shaped at all: the detector refuses those sections ("no material base table ... longest run of 20-byte records is 1", and section 209 "material record at 872 has a non-zero word after the base") |

`4956BEA0`'s single tree names its one candidate (section 43, x226), so §20/§21's reading is unchanged there. What the relocation picks do to the array is measurable: applied per tree they tile it with DISJOINT absolute index ranges on `8baf540e` - 77 -> 0..45,535; 131 -> 45,536..54,840; 165 -> 54,841..61,377; 140 -> 61,378..70,920; 190 -> 73,409..79,586; 86 -> 79,702..149,076 - with two small gaps left over (70,921..73,408 and 79,587..79,701), while §21's unit-wide table made every tree reuse the same low block. `openanniversary-work/sweep-trees.cpp` sweeps all 50 oct-tree units of the install (v14 records 6-25 MB) and compares rules by what fraction of each unit's stride-20 array its emitted triangles actually reference: mean 0.666 first-covering, 0.732 reloc-named, 0.732 for the hybrid `TerrainTableRule::relocOrFirstCovering` (named table when a tree names one, else §21's first covering run). Per-unit the hybrid never loses - `D99840D1` is the one unit where pure reloc-naming drops coverage 0.524 -> 0.458 and the fallback restores it to 0.524 - and across all 261 trees no tree names more than one candidate (`ambiguousTree=0`, `namedTableRefs>0=66`, `refsZero=195`). Reproduce: `./build/oa-asset <game-dir> terrper --hash 8BAF540E --mask FFFFFFFF`; build the two work-dir tools against `src/cdc/{Drm,Geometry}.hpp` (`g++ -std=c++23 -O2 X.cpp -I<mnt>/openanniversary/src -L<mnt>/openanniversary/build -l:liboa_cdc.a -l:liboa_core.a -lz`) and run over `/tmp/u8baf.drm` (extracted with `oa-asset extract --hash 8BAF540E --mask FFFFFFFF --out ...`) plus `<game-dir>`.

## 27. Wiring the hybrid rule into oa-game changes what a foot stands on: measured [V-data] [V-gpu]

`apps/game/main.cpp`'s `--strips=true` path now consumes `decodeTerrainUnit` under `TerrainTableRule::relocOrFirstCovering` - the same library walk §15/§20 describe, with each tree's own table applied to its chains - instead of fetching one unit-wide table, and the texture binding keys runs by (table section, matIdx) because the same material index means different pages under different tables. Measured on `8baf540e` (2026-10-23): `--strips=true --textures=true --selfCheck=true --frames=1` draws 124,503 of 135,914 chained triangles with only 11,411 gated by §18's edge rule (1,620 leaf references reached 4,776 records over 149,077 vertices), the terrain grid grows from §22's 159x29 cells over y 1,978..16,380 to 416x548 cells spanning 32,594 x 42,950 units with y -6,619..16,380, and the avatar census answers 142 of 2,304 sampled columns (the base-0 figure §19 recorded on this unit was 106). Textures: 179 (table,material) runs group into batches over 61 distinct PCD9 pages; 16 runs name a class-32 page id that matches no stored type-5 section - §21's open namespace question measured per table now. `4956BEA0 --strips=true --avatar=true --selfCheck=true --frames=1` reproduces §22 exactly (715/2,304 columns, 52,613 of 53,154 triangles drawn, 541 gated) - where §21 already worked the hybrid rule is a strict superset with identical output. What stays open: the two index gaps and the three trees whose payloads name no table (§13's absolute reading still applies to them), and whether those coarse trees are LOD rather than surface geometry.

## 28. The same traversal scan on the new geometry answers §27's open item about the walker: measured [V-data]

`--avatar=true --avatarTraversal=true --gapEnvelope=true` over `8baf540e` (2026-10-23) records 161 continuous runway+shore edges, 150 of which close inside the probe cap with widths p50 794 / p90 921 / max 931 units, 14 clearable in one flat-ground jump at run speed (reach 445 less take-off lead 20), and 11 uncapped edges that read as open water to a canyon across. §24's row for this unit under the old single table was 39 edges with p50-p90-max 1,562-1,832-1,851 and ONE clearable edge at reach 426 - so §27's per-tree tables quadruple the designed-jump evidence on this level and pull its typical void down from ~1.5x the model's reach to comfortably inside it [V-data]. The fixed walker script answers differently for the same reason: with the grid spanning 32,594 x 42,950 units instead of one block, a straight eastward walk from the densest 8/8 spot leaves the supported patch sooner (34 supported ticks before falling through), which is coverage sparsity between groups - the two index gaps and the three table-less trees of §27 - not new wrong geometry; `--avatarTraversal=true` still certifies a real crossing on this unit, now at a 794-unit-class void rather than one lucky edge. What would settle it: whether those coarse trees are LOD (§28's [V-exe] read pending in the engine's terrain-group constructor).
Settled same day by §29 [V-exe]: there is no terrain LOD in the shipped engine - visibility is sphere-versus-frustum only, so those coarse trees are ordinary surface geometry.

## 29. Every terrain tree's base table is recorded in a terrain GROUP - and the engine never swaps levels of detail [V-exe] [V-data]

tra.exe's group loop `FUN_0046f5f0` walks `Terrain + 0x18` as records at stride 0xB0 (count `Terrain + 0x14`) and skips a group only on flag bits (`(flags & 1) == 0 && (flags & 0x20000) != 0` takes the draw path; a second pass `FUN_0046f6b0` draws only groups with `flags & 0x400000`). `FUN_0040d390` (DRAW_DrawTerrainGroup) stores the group (`DAT_0069aaf4 = param_2`), clears its material lists through `FUN_0040b930` over `*(group + 0x90)`, then walks the oct-tree rooted at `param_2[0x11]` (+0x44) with `FUN_0040d170`/`FUN_0040d0d0`. Those tests are sphere-versus-frustum ONLY - a leaf is `fVar4 == 0.0`, wholly-inside nodes recurse untested, and NOTHING reads depth, cell size or distance: there is no LOD machinery for terrain in the shipped engine [V-exe], which refutes §27's "those coarse trees might be LOD" hedge as well as it settles that culling means per-node frustum tests.

Which table a strip uses is recorded data, not a census guess: `DrawTerrainStrip` (`FUN_0040b970`, ESI = the group from 0x40d12f/0x40d2c4) indexes `*(group + 0x90)` as `{u32 numMaterials; stride-20 records}` at `rec = list + 4 + matIdx * 0x14`, appends the strip to that record's array (`+0xC` count, pointer table after), and advances through `strip[10]` (+40). The base applies at draw time in `FUN_0040bae0`: `iVar2 = (strip[4] == -1) ? *(matRec + 8) : 0`, then `DrawIndexedPrimitive(dev, TRIANGLELIST, vbBase + iVar2, ...)` over the ONE vertex buffer for the whole unit (`*(terrain + 0x50)`), with indices copied unmodified by `FUN_0040cd50`/`FUN_0040b9b0`.

Measured on `8baf540e` through that layout: the Terrain struct sits in section 160 at +0x2450, its count is 9 and its array pointer relocates to section 212 (1,760 bytes = 10 x 0xB0; the tenth record has ID -1, no oct-tree, and lies outside the declared nine - the signal group). All nine groups carry flags 0, so every one passes both gates. The pairs: tree 77 -> table section 141, 86 -> 160, 131 -> 206, 140 -> 153, 165 -> 191, 190 -> 188, 197 -> 218, 200 -> 209, 215 -> 215 - six of these are exactly what §26's per-payload reloc counts and §27's hybrid already picked (the `groupRecord` rule reproduces those trees byte-identically: same heads/records/broken per tree in the terrper table), while sections 218/209/215 name the three tables §20's detector never accepted. The group records' own +0x48 and +0x90 slots both point INSIDE their table section at different addends (group 0: 141@0x2668 and 141@0x150), so the scanner takes the pair from +0x44/+0x90 and records the offset without yet trusting it.

Read through the pseudocode's OWN offsets (rec = list + 4 + matIdx * 0x14, base at rec + 8) the framing resolves on first try [V-data]: section 141@336 is count 92 then stride-20 records whose +8 words run 0..32,020 monotonically - §29's per-material column verbatim. terrper's `groupRecord` rows on `8baf540e` (2026-10-23): all nine trees land on their linked table with badHeads = unknownMaterials = 0, and the index ranges TILE the whole unit with no gaps: 77 -> 0..45,535, 131 -> 45,536..54,840, 165 -> 54,841..61,377, 140 -> 61,378..70,920, 197 (table 218) -> 70,921..73,174, 200 (209) -> 73,175..73,408, 190 (188) -> 73,409..79,586, 215 (its own section at the group's +0x90 addend) -> 79,587..79,701, 86 (160) -> 79,702..149,076. §26's two index gaps are closed by those three tables' own bytes - under the shipped pairing nothing on this unit stays base-free [V-data].

## 30. `oa-game` under the shipped pairing - measured [V-data] [V-gpu]

`groupRecord` is wired into `decodeTerrainUnit` and `oa-game --strips=true` (2026-10-23, RX 7900 XTX, Wayland):

- `--mesh=8baf540e --strips=true`: "9 terrain tree(s): 1620 leaf reference(s) reached 4776 record(s), drawing 125,347 of 135,914 triangle(s) over 149,077 vertex/vertices (10,567 gated by the edge rule, 0 bad head(s), 100 broken link(s))" - hybrid drew 124,503; every tree's index block lands in §29's tiling table with zero bad heads.
- `--avatar=true` on the same unit: grid "125,347 triangle(s), cell 79.1 units, 565x544 cells, y -17809..17810"; avatar census "148 of 2304 sampled column(s) supported across 44612x42950 units; densest spot scores 8/8 at (6153.0, -8357.5) ground 664.4"; `--avatarTraversal=true --gapEnvelope=true`: "175 edges ... 164 voids close within the 949-unit probe cap - p50 771 p90 929 max 939 units - 14 of 164 clearable in one flat-ground jump at run speed (reach 445 less take-off lead 20); 11 uncapped edges are open water".
- `--mesh=4956BEA0` is unchanged by the switch: its single tree keeps §22/§27's numbers exactly (52,613 of 53,154 triangles, grid 120x58 cells y -17656..17731, census 715 of 2,304 columns) - the group-array scan finds no Terrain struct there, so the hybrid fallback answers, and nothing regressed on units without a shipped pairing [V-data].
- The three linked-but-undetected tables (sections 218/209/215 on `8baf540e`) draw with WHITE-bound materials: §20's detected-run fetch has no class/value words to key for them, so their strips keep the default page until question (b)'s second namespace is pinned. The texture-binding warnings list on this unit drops to the same 16 class-32 ids as before [V-gpu].

Commands: `./build/oa-game --gameDir="<install>" --mesh=8baf540e --strips=true --avatar=true --selfCheck=true --frames=1` and the terrper pairing rows via `./build/oa-asset "<install>" terrper --hash 8baf540e` (the `group-record` block at its tail).

## 31. Frustum culling of the material batches ships in `oa-game` - measured [V-data] [V-gpu]

Milestone 4's open clause said "culling - every chain draws whether seen or not"; §29's [V-exe] read now says what the shipped test IS (sphere-versus-frustum only, no distance/depth/LOD swap), so `VkRenderer::drawMeshGeometry()` does exactly that per material batch:

- `uploadMesh` computes each batch's bounding sphere from its own run's vertices in the uploaded stride-20 records - box centre plus a radius out to the corner, i.e. deliberately over-approximating so nothing visible can be dropped [V-data].
- The six planes are read straight off `buildMeshMatrix()`'s world->clip matrix - the very one pushed as the vertex constant in the same command buffer - so culling and shading cannot drift apart; near is `z >= 0` because this pass maps depth into [0,1], not [-w,w] (the comment at `pushMeshMatrix` records that convention).
- Only a perspective view culls: with `--fov=0` the orthographic fit IS the probe framing, so every dump/offscreen path §6 and §23 document stays byte-identical (measured below).
- Measured on `8baf540e --strips=true --textures=true` (179 batches / 60 pages, 2026-10-23): orthographic self-check run "179 batch(es) drawn, 0 culled of 179"; centred `--fov=60` "168 drawn, 11 culled"; `--fov=60 --panX=-15000` "57 drawn, 122 culled"; `--fov=45 --zoom=0.25 --yaw=2.0` "53 drawn, 126 culled" - counts move with the view in every direction, and a batch straddling any plane stays drawn because its sphere is tested against `-radius`, not the centre [V-gpu].
- Granularity honesty: the shipped walk tests oct-tree nodes and terrain GROUPS (§29's recursion), while this culls per material run - coarser, so `oa-game` draws a superset of what the engine would keep, never a subset. Per-node culling needs the tree's own cell bounds at draw time, which the renderer does not carry yet; that is the upgrade path if batch-level counts ever stop being enough.

Reproduce: `./build/oa-game --gameDir="<install>" --mesh=8baf540e --strips=true --textures=true --fov=60 --panX=-15000 --selfCheck=false --frames=1` then read the "mesh cull on the last frame" line.

## 32. `oa-game` boots into the game's own opening area with only a `--gameDir` **[V-data] [V-gpu]**

The scene no longer needs a hash on the command line: with `--gameDir` set and neither `--mesh` nor `--level`,
`apps/game/main.cpp` opens `pc-w\generalbank.drm`, walks §6.1's 400-byte rows from section-0 image offset 4,388 and
takes the code at +0x20 off the row keyed `chapter0`. On this install that reads back as `ma1` - the chapter table's
documented start (Croft Manor) **[V-data]** - so the mesh path becomes `pc-w\ma1.drm` (`0x5526C48B`, CONTAINERS.md §6.1),
and the boot path also switches on the strip walk and material pages so one flagless run draws real shipped surfaces.

Measured 2026-10-23 with `--selfCheck=false --frames=3`: "boot level: 'pc-w\ma1.drm' (ma1)", then
"2 terrain tree(s): 428 leaf reference(s) reached 1223 record(s), drawing 31481 of 31594 triangle(s) over 40610 vertex/vertices",
then "86 material(s) grouped into 86 batch(es) over 40 distinct PCD9 page(s)" with one warn - material 78 names page id
9972 the unit does not store, exactly §30's open question b, and that batch draws its shipped vertex colour instead. The
fallback general-section draw reports "40610 vertices, 13536 triangles" and all "86 batch(es) drawn, 0 culled of 86"; three
frames present on the RX 7900 XTX **[V-gpu]**.

What this is NOT: no spawn position or camera framing comes from the table - §6.1's own honesty line says the level rows'
other fields stay open, so the camera keeps this file's documented default (orthographic fit at yaw 0.70 pitch 0.40 zoom
1.00 unless `--fov`/`--pan` flags say otherwise). Nothing is culled by winding either: `createMeshPipeline()` keeps
`VK_CULL_MODE_NONE`, because a guessed cull plane would delete half of every level before it exposed the order - the
frustum test (§31) is the only face-level removal that ships.

Reproduce: `./build/oa-game --gameDir="<install>" --selfCheck=false --frames=3` and read the "boot level:" line;
`scripts/ci.sh` runs exactly that whenever an install is present and fails on a missing "boot level:".
