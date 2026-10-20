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

## 9. The level oct-tree and its strip indices, measured [V-data]

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

Those blocks are **i16 triangle-strip index lists into section 0's stride-20 vertex array**: 496 of 530 blocks contain
only values below 149,077 (the vertex count established in §2) with more than half non-zero, and 529 of 530 satisfy the
strip property that every consecutive triple shares an index with its predecessor. Repeated indices inside a run mark the
degenerate restarts between strips, e.g. `636, 636, 640, 649, 650, 649, 640, 640, 642` **[V-data]**. So §5's first open
question - how triangles index the vertex records and where the oct-tree sits - is answered for level units; §8's
expectation that strips sit in one particular payload is what was wrong: they are spread over many small sections and
reached through leaf relocations.

Still open: which material or texture page a leaf draws with. 622 of section 86's relocations point at section 87, whose
payload begins `PCD9`/`DXT5` (§TEXTURES), so the oct-tree references at least one texture page directly; the per-leaf
material fields sit in payload regions the walk does not visit. Rendering these strips with textures is the next step for
Milestone 4.

Note on `--section`: the directory prints section indices in **decimal** while `--section` parses **hexadecimal**, so
section 86 is fetched with `--section 56`. The relocation list of a section only appears when that section is selected.
