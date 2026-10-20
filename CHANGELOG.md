# Changelog

All notable changes to this project are documented here. The format follows Keep a Changelog; the project starts at
zero version and has no releases yet.

## [Unreleased]

### Added

- CMake project (C++23, GCC 16 / Clang 23 verified) with `oa_core`, `oa_cdc`, `oa_platform`, `oa_render` libraries,
  the `oa-asset` and `oa-game` executables, and a GoogleTest target that runs without any game data.
- cdc `bigfile` container reader: index parsing with an unsorted-table refusal, ASCII-case-folded CRC-32 path
  hashing (poly `0x04C11DB7`, init/final `0xFFFFFFFF`, non-reflected), sector/part addressing with the
  Legend/Anniversary alignment `0x9600000`, specialisation-mask record selection, zlib and raw-deflate inflation for
  compressed records, and reads that straddle a part boundary.
- `oa-asset` subcommands `list`, `hash`, `resolve <paths.txt>`, `extract --path|--hash [--mask]`,
  `drm --path|--hash [--section N --out FILE]` and `texture --path|--hash [--section N] --out FILE.png`.
- DRM v14 unit parser (`src/cdc/Drm.*`): section directory, per-section payload offsets, a `stored` flag for entries whose
  bytes an archive declares but never writes, and typed relocation entries (kind, target section index, typeSpecific, slot
  offset, plus the addend sitting at that slot) decoded from `tra.exe`'s unit loader. The payload base comes from byte-exact
  closure confirmed by every texture magic; when that arithmetic is off, the parser re-solves it from an actual `PCD9` and
  marks a suffix of directory entries unstored - level units need this (`8baf540e` declares a trailing entry of 1,288 bytes
  that its image does not store), and record `F2197DE6` parses because all nine of its textures anchor even though its
  pre-payload region is 4 bytes below the block A the header claims. Everything else still refuses rather than guesses:
  wrong version, table bounds out of the image, no candidate base that both anchors every magic and closes at end-of-image,
  or - when a unit has no magic to anchor on - a region smaller than the declared block A. Run across all 4,304 records of
  the shipped install: **all 1,098 version-14 units parse byte-exactly**, every other record is rejected by its leading
  dword (`docs/formats/CONTAINERS.md` §8 census). The earlier finding that the trailing size-0 type-32 entry carries no
  relocation bytes stands - reading it as a count over-consumes `generalbank.drm` by 56 bytes and `uigeneral.drm` by 32
  (`docs/formats/DRM.md` §2.1). Seven unit tests on synthetic units; real-install verification in `docs/TESTING.md` §3.
- PCD9 texture parser (`src/cdc/Texture.*`) with mip-chain validation, and `oa-asset texture`, which decodes mip 0 for the
  eye (BC1/BC3 CPU decode plus a small zlib PNG writer). Measured across all 71 type-5 sections of `lara.drm`: format tag at
  +4 (`DXT1` / `DXT5` / `0x15`), payload length at +8, extent at +16/+18, bits-per-pixel at +20, levels-1 at +21 and a usage
  word at +22; every chain total is byte-exact once the two smallest levels of a 512-wide DXT1 chain count as one block each.
  Tag `0x15` sections are RGBA masks whose RGB is constant white with the shape in alpha, and DXT5 section 7 carries colour in
  only five of its 5463 blocks. Section 0 decodes to Lara's clothing atlas. New page `docs/formats/TEXTURES.md`; six unit
  tests carry that unit's numbers without shipping its bytes.
- Startup plumbing: one-line UTC log records filtered by level (`src/core/Log.*`) and three-layer key/value
  configuration, defaults < file < `--key=value` command line (`src/core/Config.*`).
- Native front end (Milestone 2): SDL3 window with close/Escape handling and keyboard state (`src/platform/Window.*`),
  Vulkan renderer with validation-layer gating on `VK_EXT_debug_utils`, discrete-GPU device selection, swapchain
  lifecycle with out-of-date/suboptimal recovery, per-image fence plus acquire semaphore, and a clear render pass
  presented to the window (`src/render/vk/VkRenderer.*`).
- `oa-game`: configuration-driven startup, container smoke open, frame loop with resize handling, and
  `--selfCheck=true` which clears an offscreen image, copies it back through a host-visible buffer and compares every
  pixel against the clear colour within one byte.
- Documentation: `README.md`, `docs/ENVIRONMENT.md`, `docs/ARCHITECTURE.md`, `docs/formats/CONTAINERS.md`,
  `docs/EXISTING_PROJECT_AUDIT.md`, `docs/ROADMAP.md`, `docs/formats/DRM.md`, `docs/formats/TEXTURES.md`,
  `docs/formats/GEOMETRY.md`.
- `scripts/ci.sh`: the canonical pipeline — plain unit-test run, renderer smoke on a real Wayland session or under Xvfb
  (exit 77 skip when nothing can present), then the same suite built with `-DOA_SANITIZE=ON`. `.github/workflows/native.yml`
  mirrors it and pins SDL3 to tag `release-3.2.0`; the workflow has never executed because this repository has no remote.
- `docs/RESEARCH.md`: prior art with links and license boundaries, the PCD magic cross-check between engine generations,
  and a full `tra.exe` PE/import inventory (`objdump -p`). `docs/TESTING.md`: procedures, fixture policy, uncovered areas.
- `oa-game --texture=<record path> [--section N]`: opens the container, parses that record as a DRM v14 unit, takes the
  first `PCD9` section (or the hexadecimal one given) and uploads its whole mip chain to the GPU - DXT1 to
  `VK_FORMAT_BC1_RGBA_SRGB_BLOCK`, DXT5 to `BC3_SRGB`, tag `0x15` to `B8G8R8A8_SRGB` - then draws it as one textured quad
  with premultiplied-alpha blending and an aspect-fit push constant that letterboxes without distorting. Section numbers
  are hexadecimal, like in `oa-asset`.
- `VkRenderer::uploadTexture()` (staging buffer, one `vkCmdCopyBufferToImage` region per mip level, layout transition,
  combined-image-sampler descriptor) and `sampleInto()`, the offscreen draw-and-read-back helper the self-check uses.
- `src/cdc/Geometry.*`: the unindexed triangle-run decoder (`decodeMesh`, `meshBounds`) for a general section's stride-20
  vertex records - position i16x3, four modulate bytes, texture coordinates - with synthetic fixtures pinning every field.
- `oa-game --mesh=<record path or eight-digit hash> [--section N] [--yaw/--pitch/--zoom]`: parses that record as a DRM unit,
  picks the given section (or the largest stored type-0 one) and uploads its records verbatim; `shaders/mesh.vert` and
  `mesh.frag` draw them depth-tested against themselves with an orbit camera passed as push constants. Colours are flat:
  nothing in the archive ties the modulate bytes to an interpolation rule yet.
- The window render pass now carries a depth attachment shared by both pipelines, and `sampleInto()` mirrors it with a
  temporary depth image so every probe stays render-pass-compatible with the pipeline it draws. New
  `VkRenderer::uploadMesh()`, `renderMeshOffscreen()` (headless geometry read-back) and `createMeshPipeline()`.
- Six `Mesh.*` unit tests; `scripts/ci.sh` now also runs `--mesh` against a real install when `OA_GAME_DIR` is set
  (`OA_MESH_RECORD` overrides the level unit).

- `src/cdc/TerrainQuery.*`: a uniform-grid query over a decoded mesh returning the highest supporting surface under a foot,
  with the cell size taken from the mesh's own median edge; five data-free tests pin exact heights (slope interpolation, top
  floor of two stacked planes, a wall that gives no support, refused inputs, self-derived cell size).
  `oa-game --walk=true` drives a deterministic point agent over that query - gravity, fixed timestep, logged support per tick
  and the corners of the triangle it settles on.
- `oa_cdc::decodeTerrainOctree()` and `decodeTerrainStripChains()` (`src/cdc/Geometry.*`): the level oct-tree walked through a
  payload of self-relative nodes, and the terrain strip chains its leaves point at - 44-byte header (vertexCount at +0, matIdx
  at +20, nextTexture at +40), u16 index run into the unit's vertex array. Chains are entered only from a leaf reference and
  followed through `nextTexture`, each record emitted once however many leaves reach it; `vertexCount` must be a multiple of
  three within range, its run inside the payload and every index naming a vertex the array holds. A link that names a header
  four to eight bytes short of a real record - the archive patches those slots like any other pointer and pads odd-length runs,
  and GEOMETRY.md §16 measured 139 of 750 sampled links sitting exactly four bytes early - steps forward onto that record and
  counts the step; anything else ends the chain instead of being guessed at. Seven `TerrainStrips.*` tests pin the walk on
  synthetic payloads: a chain walked to its end, a bad head counted rather than invented, a shared tail emitted once, an index
  naming a vertex the unit does not have, a link stepped only onto a real header (and one too far ending the chain), a cycle
  walked once, and a payload holding something else.
- The off-by-one that hid those chains is fixed in the parser: `relocSectionIndex()` subtracts one from the stored value because
  the engine numbers sections from 1 (`src/cdc/Drm.hpp`, read out of `FUN_005bca80` case 5), and a stored 0 resolves to
  `kNoRelocSection` instead of wrapping around. With that numbering every self-referring slot of unit `8baf540e` lands inside its
  own payload and all 1,620 oct-tree leaf references land on a strip-record header (GEOMETRY.md §15).
- `oa-game --strips=true`: draws those chain-reached index lists instead of reading the vertex array as one unindexed run. The
  payload-wide hunt for header-shaped bytes is gone from library, app and tool; `oa-asset terrain --hash 8BAF540E` prints the
  leaf-driven walk per tree - nodes/leaves/depth, heads, records, bad heads, broken links, steps taken, triangle count, the index
  range reached and edge percentiles - and refuses to call a payload an oct-tree when its node fields say otherwise.

### Verified
- Container unit tests (through CTest; the suite is 47 cases today, these among them): golden path hashes taken from the
  shipped archive (`pc-w\generalbank.drm` → `0xB3AAAFD6`, `pc-w\lara.drm` → `0x5FF91900`), synthetic stored/zlib/
  raw-deflate records, part-boundary straddling, specialisation-mask selection, unsorted-index rejection, strict typed
  getters, log level filtering.
- `docs/formats/DRM.md`: the version-14 unit layout measured from six extracted units — header fields, the
  v19-compatible 20-byte section directory (types, sizes, ids, spec masks), 8-byte relocation entries proven by landing
  all 71 texture payloads of `lara.drm` on their magic while consuming the file exactly, and the unresolved regions.
- Real install (`tra.exe` sha256 `b43ade0d922b0f007747e5707b97c86404cbf4f051829e3711f7007a656ea53a`, GOG build):
  4,304 records across 27 parts; hash table sorted; no record straddles a part boundary; every PC record stored
  uncompressed. Name recovery resolves 948 of the 949 object-list units as `pc-w\<name>.drm`. Extraction round trip:
  `pc-w\generalbank.drm` (90,384 bytes) and `pc-w\lara.drm` (7,630,042 bytes), both starting with DRM version 14.
- Native run on this machine, no Wine/Proton, KDE Wayland session, AMD RX 7900 XTX (RADV NAVI31):

  ```sh
  ./build/oa-game --selfCheck=true --frames=600 \
      --gameDir="/mnt/2nd4TB/TombRaiderAnniversary/Tomb Raider Anniversary"
  # swapchain 1600x900 images=4 format=50 presentMode=2
  # self-check passed: 64x64 read-back matches the clear colour
  # presented 600 frame(s) on 'AMD Radeon RX 7900 XTX (RADV NAVI31)' in 3.799s (157.9 fps)
  ```

  A desktop capture taken during that run shows the window filled with exactly the clear colour: the configured linear
  `(0.10, 0.35, 0.62)` is stored as sRGB `#59A0CF`, and the largest solid block of that colour in the grab measures
  exactly 1600x893 px at (1120, 649) — the requested client size at scale factor 1, a few bottom rows hidden by the
  desktop panel. With validation enabled the only reported messages are the host's
  stale `liblsfg-vk-layer.so` loader entries described in `docs/ENVIRONMENT.md`.

- `scripts/ci.sh` executed end-to-end here (with `OA_GAME_DIR` pointing at the real install): plain suite 33/33; renderer
  smoke on Wayland with the RX 7900 XTX reported both self-check phases, drew one shipped triangle run and presented 3
  frames; ASan+UBSan suite 33/33. Under Xvfb without DRI3 the same step exits 77 as a documented skip rather than failing.
- Real GPU texture path with shipped bytes only (`pc-w\lara.drm` section 0: 512x512 DXT1, 10 levels, 174,776 payload
  bytes): `oa-game --gameDir=... --texture='pc-w\lara.drm' --selfCheck=false` logs
  `uploaded 512x512 texture: format=134 levels=10 bytes=174776`, and a desktop grab during that run shows Lara's clothing
  atlas as a centred square one drawable-height wide with the clear colour either side, in the same layout
  `oa-asset texture` decodes on the CPU - container, DRM section directory, PCD9 mip chain, BC1 upload, sampling and
  blend agree at both ends [V-screen].
- Self-check phase two: a synthetic 16x16 `B8G8R8A8_SRGB` pattern goes through `uploadTexture`, the swapchain pipeline,
  premultiplied blending and an offscreen attachment that is render-pass-compatible with it, then returns byte-for-byte
  within one unit (`self-check passed: sampled 16x16 texture matches the uploaded texels`). Phase one keeps its plain
  `R8G8B8A8_UNORM` image so clear floats stay linear. Validation reports nothing beyond the host's stale loader entries,
  and no object leaks at teardown.
- Shipped geometry on the GPU, container to pixels (`docs/formats/GEOMETRY.md` §6): `oa-game --gameDir=...
  --mesh=8baf540e --selfCheck=true` logs `drawing section 0 of '8baf540e' (id 0, type 0): 149077 vertices, 49692
  triangles, bounds 55103x55225x50366 world units` — the vertex count is exactly that section's 2,981,540 payload bytes
  over 20 — and `mesh probe: 6768/9216 pixels covered by 12829 distinct vertex colours, 0 unexplained`, i.e. every lit
  pixel of the offscreen read-back equals one shipped colour byte within an sRGB step. A desktop capture of a 900x600
  window during that run shows the level silhouette with facet colours and depth occlusion; it is evidence of shape, not
  of bytes, because this session's compositor remaps window pixels (no window pixel equalled a shipped byte exactly).
  Validation reports nothing beyond the host's stale loader entries [V-gpu].

- Type-7 sections characterised over the whole install (`docs/formats/GEOMETRY.md` §7): 37,871 sections in 1,086 of the
  1,098 version-14 units, 13,196,830 bytes, sizes 4 to 368,400 B with 53.7% exactly 48 B. The directory `id` is a key:
  8,458 distinct ids, each holding one payload size archive-wide (id `0x6d1`: 128 B in 192 units) and unique within a unit,
  with payloads of a class byte-identical between units. Contents are either records carrying an ASCII resource path
  (`Event_triggered\time_bend_03.`) or float-bearing blobs without a uniform stride - never vertex arrays - and the slot-7
  handler in `tra.exe` (`FUN_005bc830`, vtable `PTR_FUN_00657338`) keeps them in a 32,768-slot refcounted cache (block of
  `size + 0x10`, u16 refcount at +0, data at +0x10). Type 7 is shared resource instances, not model geometry; the handler
  table itself resolves as `DAT_0088e2b0 + type*4` with slot 5 being the texture handler.
- Level oct-tree decoded from shipped bytes (`docs/formats/GEOMETRY.md` §9): unit `8baf540e` section 86 holds a node array
  of `{bounding sphere (4 floats), leaf reference u32 at +16, child count s32 at +20, self-relative u32 child offsets}`;
  walking from offset 0 visits 623 nodes / 533 leaves to depth 6 with no anomalies. All 533 leaf reference slots are
  relocation sites pointing into 34 sibling sections (16 leaves each, addends ascending from 0). Reading the blocks between
  consecutive addends as i16 triangle-strip lists into section 0's stride-20 vertex array has now been measured against
  geometry and refuted: those runs index nothing coherent - RMS longest edge is 20-64% of the array span for all nine of
  the unit's oct-trees - so what a leaf reference addresses is open again.
- Terrain strip records located inside those payloads (`docs/formats/GEOMETRY.md` §10): cdcEngine tr7 (GPL-3.0, consulted
  as documentation only; `openanniversary-work/priorart/cdcEngine`) describes `TerrainTextureStripInfo`, and the file form
  matches it - a 44-byte header (vertexCount i32 at +0, matIdx i32 at +20, nextTexture u32 at +40) followed by
  `vertexCount` little-endian u16 indices into section 0. Measured over unit `8baf540e` with
  `oa-asset <game-dir> terrain --hash 8BAF540E`: 4,284 records in 101 payloads; 2,827 of their `nextTexture` values land on
  another accepted record start inside the same payload, so that field is an absolute offset within a payload; the triangles
  are coherent at ~0.4% RMS. They index 15,724 of 149,077 vertices, and only 85 of 1,620 leaf addends land on a record
  start, so leaf-to-record linkage, the other payload kinds and the rest of the vertex array stay unresolved.
- Per-type handler table named (`docs/formats/GEOMETRY.md` §11): `FUN_005bd980` fills `DAT_0088e2b0 + type*4` with one object
  per section type - types 0, 4 and 10 share `PTR_LAB_00657444`, type 5 is the texture handler returned by `FUN_00403b30`,
  type 7 `FUN_005bc830` - so a level unit's terrain payloads are loaded through one generic stride-20 object path
  (`FUN_005bca80`'s default branch: count at +0x14, allocate `count * 0x14`, copy in stride-20 steps) and no terrain-specific
  handler exists in that table; what a leaf reference means is decided by game code after loading.
- Leaf-addend header probe over unit `8baf540e` section 86 (`docs/formats/GEOMETRY.md` §10): a record header parses cleanly at
  only **50 of 533** leaf addends, and the ones that do parse chain normally (target 183, addend 0: vertexCount 384, matIdx 75,
  nextTexture 0x32C), so the record form is confirmed while leaf-to-record linkage stays unresolved.
- Handler vtables read out of `tra.exe` itself and the stride-20 record fields named (`docs/formats/GEOMETRY.md` §12): the PE
  has two sections, so a virtual address maps as `0x200 + va - 0x401000`; twelve-slot handler vtables lie back to back at
  `0x657414`, `0x657444` (types 0/4/10), `0x657474` (type 2) and `0x6574a4`. The type-0 handler's slots +0/+C are a payload
  free (`FUN_005bc880`) and a word-at-a-time copy into the object buffer (`FUN_005bc8a0`), and type 2 copies into the static
  table `DAT_0084d41c[idx*3]` - the container layer allocates, copies and frees; it interprets nothing. The stride-0x14 walk
  belongs to the object-instance loader (`FUN_005bca80`, whose version check quotes `Wrong mkloadob version data %x, c=%x`)
  and names instance fields instead: key u32 at +0, resource-type byte at +4 that indexes `DAT_0088e2b0 + type*4`, flag byte
  at +5 with bit 0 marking merged records, a u16 argument at +6, a grouping id u32 at +C and a mask u32 at +0x10; resolved
  handles leave as eight-byte {u32 handle, u16 type} entries. Eight of those functions were absent from the earlier Ghidra
  export, so `tools/ghidra/scripts/DecompileFunctions.java` now creates a function at an address when none exists and they
  were re-exported into `openanniversary-work/ghidra/decomp`.
- Terrain chains reached through the tree that indexes them, drawn indexed (`docs/formats/GEOMETRY.md` §15, §16): nine payloads
  of unit `8baf540e` hold oct-trees (sections 77, 86, 131, 140, 165, 190, 197, 200, 215) and all 1,620 of their leaf references
  land on a strip-record header once `relocSectionIndex()` subtracts the engine's one-based numbering. Before that fix the same
  walk reported a broken link for every chain (broken equalled leaves in all nine trees); after it, 4,776 records are reached with
  `badHeads` 0 - 1,511 links stepping four bytes onto their record and 109 naming nothing valid within two words and ending a
  chain. What those chains index is exactly the contiguous front of section 0: 16,338 distinct vertices, largest index 16,337. On
  AMD RX 7900 XTX (RADV NAVI31) with validation on, `oa-game --gameDir=... --mesh=8baf540e --selfCheck=true --strips=true` printed
  `9 terrain tree(s): 1620 leaf reference(s) reached 4776 record(s), drawing 135914 triangle(s) over 149077 vertex/vertices;
  0 bad head(s), 109 broken link(s), 1511 link(s) stepped onto their record` with the probe `mesh probe: 2194/9216 pixels covered
  by 12829 distinct vertex colours, 0 unexplained`, and `--walk=true` added `terrain grid: 135914 triangle(s), cell 213.5 units,
  194x172 cells, y -4534..27613` plus `walk finished: 773 of 900 tick(s) supported`. The agent settles on a record that is coarse
  throughout - §16 measures two such trees, median longest edges 9,926 and 6,934 world units against 63-206 for the other seven -
  which is why `--walk=true` logs at Warn level that these heights measure the decode rather than shipped ground.
  `oa-asset <install> terrain --hash 8BAF540E` reproduces every figure above, and `scripts/ci.sh` passes both game flags whenever
  `OA_GAME_DIR` is set.

### Fixed

- Teardown order on early-exit paths (Vulkan init failure, self-check failure): the SDL window and video subsystem were
  destroyed while a `VkSurfaceKHR` was still alive, so validation reported `vkDestroyInstance(): … has 1 leaked objects`,
  naming the surface. One RAII cleanup object now fixes the order for every exit path. Measured under Xvfb (no DRI3):
  the same failing path printed the leak before the change and prints none after.
- `createPipeline()` destroyed both shader modules whenever it created the descriptor set layout instead of only on
  failure, so `vkCreateGraphicsPipelines` received invalid handles: validation reported `Invalid VkShaderModule Object`
  and RADV then segfaulted inside pipeline creation.
- The texture descriptor write left a null sampler in `VkDescriptorImageInfo`, whose first field is the sampler; that
  invalidated the combined-image write and crashed the draw behind it.
- Aspect fit scaled the wrong axis: a square image in a wide window drew as a 2240x709 band instead of a centred
  1260x1260 square. The comparatively wider axis now shrinks.
- `VkPipelineViewportStateCreateInfo` carried `pDynamicStates`/`dynamicStateCount`, which belong to
  `VkPipelineDynamicStateCreateInfo`; that struct is now built and bound to the pipeline. The sampler also set a
  nonexistent `mipLodMaxLevel` next to the real `maxLod`.
- The graphics pipeline was never rebuilt after swapchain recreation, so a resize left the frame loop without one; it is
  recreated together with its render pass. `VkDescriptorSetLayout` leaked until `vkDestroyDevice`.

### Removed

- `decodeTerrainStrips()` from `oa_cdc` with its tests: its model - that an oct-tree leaf's target section holds one u16
  index run reaching to the next leaf's offset - is refuted by §10 of `docs/formats/GEOMETRY.md`. The same segmentation now
  lives in `oa-asset terrain` as a labelled probe, which is what prints the coherence figures that refute it.
- The oct-tree-driven triangle path in `oa-game`: it indexed vertices through those refuted blocks and so drew spikes
  instead of level geometry. A level unit's stride-20 array again draws as one unindexed run until §10's open questions are
  settled; `VkRenderer` keeps the indexed-drawing support its `MeshUpload::indices` field gained, unused until a real
  index source exists.

### Not yet implemented

- Geometry beyond textures: model vertex and index buffers (level-unit type-0 runs draw, §6 above), materials, transforms
  and camera controls; the type-10 payloads of a level unit are still uncharacterised (Milestone 4).
- Gameplay systems, level streaming, audio and cinematics (Milestones 5-6), plus any distribution work (Milestone 8).
