# Architecture

The design goal is a native Linux reimplementation that reads the user's own installation and never needs Wine,
Proton or the original executable at runtime. Everything below follows from that: small layers, one direction of
dependency, and format knowledge isolated behind parsers with tests.

## Layers and dependency direction

```
apps/ (oa-game)    ← composition only: config, logging, loop; no format or API details
  ├── src/render/vk      Vulkan renderer: instance → surface → device → swapchain → passes
  ├── src/platform       SDL3 window + input; the only place that knows about a display server
  ├── src/formats        per-format parsers (DRM v14 units, textures, sound …) — pure data in/out
  ├── src/cdc            container layer: bigfile index, path hashing, record selection/decompression
  └── src/core           logging, configuration; no dependencies on any of the above
```

A target may depend only on layers below it. `src/formats` and `src/cdc` never include SDL or Vulkan headers, which
keeps every parser testable without a display: that is what makes Milestone 3's "validated with repeatable tests"
possible and is the reason `oa_tests` currently runs in 10 ms.

## Conventions this repository follows

* **Error handling.** No exceptions across module boundaries: `bool` return plus `std::string* error`, or a
  `std::optional` for lookups. The container, config and renderer all use the same shape.
* **Evidence before code.** Every format claim in `docs/formats/*` carries an address (`FUN_0047ddf0`), an offset, a
  reproducible command, or a citation, tagged **[V-exe]** (our own disassembly), **[V-data]** (reproduced against the
  shipped install), **[S]** (secondary source) or **[I]** (inference).
* **Tests run without game data.** Synthetic fixtures plus golden constants taken from the real archive. Anything that
  genuinely needs the install goes behind a skip, never into the default suite.
* **Read-only asset access.** Files are opened `"rb"`; extracted payloads go to a scratch directory outside both the
  install and this repository.
* **No speculative abstraction.** One implementation means one concrete type: no interfaces or factories until a second
  implementation exists (the renderer has exactly one backend today).

## Runtime shape (Milestone 2, current)

`apps/game/main.cpp` reads configuration (defaults < `openanniversary.ini`/`$OA_CONFIG` < `--key=value`), opens the
container if `gameDir` is set (logging record and part counts as a smoke test of the asset path), creates an SDL3
window, initialises Vulkan with the validation layer when available, then loops: pump events → resize check → clear
pass into a swapchain image → present. The frame loop uses one fence plus one acquire semaphore per swapchain image,
which is enough to keep exactly one frame in flight while still overlapping acquisition and submission.

`VkRenderer::selfCheck()` proves the device path independent of what is on screen: it clears an offscreen
`R8G8B8A8_UNORM` image, copies it into host-visible memory and compares every pixel against the clear colour within a
rounding tolerance of one byte. That is the repeatable test hook CI uses; the swapchain path is exercised by counting
presented frames.

## Deliberate omissions so far

No ECS, no scene graph, no material system, no RHI abstraction and no shader compiler pipeline yet — those are pulled
in when DRM v14 units (geometry, materials, transforms) actually demand them, not before. The first draw commands will
land in `src/render/vk` together with the vertex/index buffers that `src/formats` produces.

## Licensing boundary

GPL-3.0-or-later (`LICENSE`). Code from the other local ports may be copied only after their owner adds an explicit
license grant; see `docs/EXISTING_PROJECT_AUDIT.md` for which trees are GPL-compatible today and which grant nothing.
No proprietary bytes, decompilations or extracted game data enter git (`.gitignore` enforces it); reverse-engineering
scratch work lives in the sibling `openanniversary-work/` directory.
