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
  ├── src/game           body over geometry: gravity, ground contact with step-up, jump; depends only on `src/cdc`'s query
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
container if `gameDir` is set (logging record and part counts as a smoke test of the asset path) and, when `--texture`
names a record, parses it as a DRM v14 unit far enough to upload one PCD9 mip chain; `--mesh=<record-or-hash>` does the
same walk for a general section and uploads its stride-20 vertex records unchanged (`docs/formats/GEOMETRY.md` §6). Then
it creates an SDL3 window, initialises Vulkan with the validation layer when available, and loops: pump events → resize
check → one pass into a swapchain image that first draws the uploaded triangle run (depth-tested against itself, orbit
camera through push constants) and then the textured quad over it (premultiplied alpha, one push constant for the
aspect-fit letterbox) → present. The frame loop uses one fence plus one acquire semaphore per swapchain image, which is
enough to keep exactly one frame in flight while still overlapping acquisition and submission.

`VkRenderer::selfCheck()` proves the device path independent of what is on screen, in two phases. It clears an offscreen
`R8G8B8A8_UNORM` image (unorm keeps clear floats linear) and compares every read-back pixel against the clear colour
within one byte; then it uploads a synthetic `B8G8R8A8_SRGB` texture through `uploadTexture()` and samples it with the
*swapchain pipeline* into an offscreen attachment that is render-pass-compatible with it, comparing bytes again. The
second phase is what catches descriptor, layout-transition and blend mistakes, and its attachment format has to equal the
pipeline's colour format or the draw is invalid. That pair is the repeatable test hook CI uses; the swapchain path is
exercised by counting presented frames. A resize recreates swapchain, render pass and pipeline together, because a
pipeline belongs to the pass it was built against.

Geometry gets the same offscreen treatment from `VkRenderer::renderMeshOffscreen()`: it draws the uploaded run into a
96x96 attachment with `sampleInto(…, drawMesh = true)` and reads the pixels back, so a headless run can require that the
shipped positions and colours really reach pixels. Because both pipelines were built against the window pass, every probe
pass declares the same two attachments (colour plus a temporary depth image) — render-pass compatibility is what makes the
draw valid, not an implementation detail of one pipeline.

## Deliberate omissions so far

No ECS, no scene graph, no material system, no RHI abstraction and no shader compiler pipeline yet — those are pulled
in when DRM v14 units (geometry, materials, transforms) actually demand them, not before. The first draw commands will
land in `src/render/vk` together with the vertex/index buffers that `src/formats` produces.

## Licensing boundary

GPL-3.0-or-later (`LICENSE`). Code from the other local ports may be copied only after their owner adds an explicit
license grant; see `docs/EXISTING_PROJECT_AUDIT.md` for which trees are GPL-compatible today and which grant nothing.
No proprietary bytes, decompilations or extracted game data enter git (`.gitignore` enforces it); reverse-engineering
scratch work lives in the sibling `openanniversary-work/` directory.
