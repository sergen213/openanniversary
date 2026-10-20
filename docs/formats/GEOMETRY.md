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
should was untested until a point agent was walked over the decoded triangles (`TerrainQuery`, `--walk=true`). It is not.

    oa-game --gameDir="<install>" --mesh=8baf540e --strips=true --walk=true --frames=1
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
no linking rule can fix; `--walk=true` still settles on one of them (again an edge of ~13.8k units), so the app keeps
saying at Warn level that walked heights measure this decode rather than shipped ground. §17 measures what does and does not
explain those records.

## 17. No per-material base is stored in the unit, and geometry alone cannot choose one, measured [V-data]

The coarse records of §16 look like an unapplied vertex base: prior art draws a strip with `vbBaseOffset + index`
(`tr7/game/pc/D3D/d3dterrain.cpp:244`, the field living in `XboxPcMaterialList` at `tr7/game/terrain.h:67-74`), and that list is
built at run time - no assignment to `vbBaseOffset` exists anywhere in cdcEngine. Three measurements on unit `8baf540e` say what
the shipped data does not give us.

1. **The bases are not in the file.** For 79 per-(strip payload, material) offsets that make a group's triangles short (searched
   across all of section 0), scanning every stored payload for those values as u32 words finds them nowhere, and exactly one
   relocation entry in the whole unit names section 0: site s160+9360, kind 0, addend 0. Those offsets are computed by the loader,
   not stored beside the strips they apply to [V-data].
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

So the decoder keeps what it has: indices are absolute into section 0, which reads coherently for the fine trees and badly for
the coarse ones, and `--walk=true` stays a Warn-level probe rather than ground truth. What would close this is not more
statistics but the one thing these measurements cannot produce - the code in `tra.exe` that builds a terrain group's vertex
buffer as its unit loads, turning data we already parse into per-material bases. That is where Milestone 4's geometry work goes
next [S].
