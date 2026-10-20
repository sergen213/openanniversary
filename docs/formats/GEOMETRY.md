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

1. Decompile the per-type handler objects reached from the loader's handler table (`DAT_0088e2b0`; case 5 of
   `FUN_005bca80` calls `handler + 0x10` after relocations are applied [V-exe]) and find the loop that walks a vertex
   base with stride `0x14`. That gives record order, index encoding and the oct-tree walk for free.
2. Cross-check against the community level viewer's claims — 0xB0-byte oct-tree nodes, triangle ranges per material via
   `vbBaseOffset`/`polygonStart`/`polygonCount`, model header fields `_octTreeCount` at +0x14 and `_vertexStart` at
   +0x44 [S]. Those numbers come from a tool for *Legend*; Anniversary's own units must reproduce them before they are
   treated as fact, and blind fitting of those offsets over `8baf540e` produced only sliding-window artefacts [V-data].

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
