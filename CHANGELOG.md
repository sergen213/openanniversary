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

### Verified
- Container unit tests (12 passing through CTest): golden path hashes taken from the shipped archive
  (`pc-w\generalbank.drm` → `0xB3AAAFD6`, `pc-w\lara.drm` → `0x5FF91900`), synthetic stored/zlib/raw-deflate records,
  part-boundary straddling, specialisation-mask selection, unsorted-index rejection, strict typed getters, log level
  filtering.
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

- `scripts/ci.sh` executed end-to-end here: plain suite 12/12; renderer smoke on Wayland with the RX 7900 XTX reported
  "self-check passed" and presented 3 frames; ASan+UBSan suite 12/12. Under Xvfb without DRI3 the same step exits 77 as a
  documented skip rather than failing.
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

### Not yet implemented

- Geometry beyond textures: the type-7 section layout, vertex and index buffers, materials, transforms and camera
  controls (Milestone 4).
- Gameplay systems, level streaming, audio and cinematics (Milestones 5-6), plus any distribution work (Milestone 8).
