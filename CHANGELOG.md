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
- `oa-asset` subcommands `list`, `hash`, `resolve <paths.txt>` and `extract --path|--hash [--mask]`.
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
  `docs/EXISTING_PROJECT_AUDIT.md`, `docs/ROADMAP.md`.

### Verified

- Container unit tests (12 passing through CTest): golden path hashes taken from the shipped archive
  (`pc-w\generalbank.drm` → `0xB3AAAFD6`, `pc-w\lara.drm` → `0x5FF91900`), synthetic stored/zlib/raw-deflate records,
  part-boundary straddling, specialisation-mask selection, unsorted-index rejection, strict typed getters, log level
  filtering.
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
  `(0.10, 0.35, 0.62)` appears as sRGB `#59A0CF` over a 1848x1533 device-pixel area (the 1600x900 logical window at 2x
  HiDPI), matching the requested size and colour. With validation enabled the only reported messages are the host's
  stale `liblsfg-vk-layer.so` loader entries described in `docs/ENVIRONMENT.md`.

### Not yet implemented

- Any payload-level parser beyond the container: DRM v14 unit sections, textures, sound, cinematics.
- Draw calls, materials, geometry buffers, gameplay systems, audio (Milestones 4-6).
