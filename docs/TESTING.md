# Testing

Everything in this file is a command that has been executed on the development host, or is explicitly marked as not yet
available. Goal rule: never claim something works without running a relevant test (`goal.txt` §"Verification").

## 1. Canonical pipeline

```sh
scripts/ci.sh          # clean configure + build + unit tests + renderer smoke, in a throwaway directory
```

That script is the single source of truth; `.github/workflows/native.yml` runs the same steps on a runner. Local use:

```sh
OA_GAME_DIR="/mnt/2nd4TB/TombRaiderAnniversary/Tomb Raider Anniversary" OA_SANITIZE=ON scripts/ci.sh
```

What it does, in order:

1. Fresh `Debug` configure with `-DOA_BUILD_TESTS=ON`, **no** sanitizers → build → `ctest --output-on-failure`.
2. Renderer smoke under a virtual X display (`xvfb-run -a`): `oa-game --selfCheck=true --frames=3`, plus the container
   open path and one shipped triangle run (`--mesh=<hash>`, override with `OA_MESH_RECORD`) when `OA_GAME_DIR` is supplied.
   Skips with exit 77 if `xvfb-run` or a Vulkan ICD is unavailable, so a headless machine without drivers cannot report a
   false pass.
3. Second configure with `-DOA_SANITIZE=ON` (ASan+UBSan) → build → **unit tests only**. Driver/SDL allocations are known
   to leak inside vendor code, so sanitizers guard the parser/core layer, not the display path.

Verified on this host (CachyOS, GCC 16.2.1): plain suite 53/53; the renderer smoke passes against the real GPU through the
Wayland session - both self-check phases pass ("matches the clear colour", "matches the uploaded texels"), and with
`OA_GAME_DIR` set the same run draws shipped content: strip chains log 119,452 of 135,914 triangles with 16,462 gated by the
edge rule over 4,776 records reached by 1,620 leaf references, and the point agent reports 773 of 900 ticks supported; then
ASan+UBSan suite green. The same step under Xvfb without DRI3 exits 77 as a documented skip ("no presentation-capable
Vulkan device") instead of failing, since that is an environment gap rather than a regression — measured here: Xvfb reports
"No DRI3 support detected" and the renderer refuses with "no graphics+present queue with a swapchain-capable device".
Runner-side behaviour (Ubuntu image plus `mesa-vulkan-drivers`/lavapipe) is *not* verified yet — see §6.

## 2. Unit tests (`oa_tests`, GoogleTest via CTest)

```sh
cmake -S . -B build -G Ninja -DOA_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build --output-on-failure          # 53 tests, well under a second total
```

Fixture policy — the suite must pass with **no game data present**, so it is runnable in CI and on any machine:

| Area | Fixture | Golden value source |
| --- | --- | --- |
| Path hashing | ASCII-case-folded CRC-32 over `pc-w\generalbank.drm`, `pc-w\lara.drm` | hashes measured from the shipped index: `0xB3AAAFD6`, `0x5FF91900` [V-data] |
| Index parsing | synthetic 4-record table (sorted and unsorted variants) built in-test | layout from our disassembly, `docs/formats/CONTAINERS.md` §1 |
| Part addressing / straddle | two synthetic parts at the Legend alignment `0x9600000`, one record crossing the boundary | §3 of that document |
| Compression | stored, zlib and raw-deflate records generated in-test with zlib | §5 |
| Specialisation masks | mask selection incl. `0xFFFFFFFF` always-on case | §4 |
| Config / logging | defaults < file < `--key=value` precedence; level filtering writes one subsystem line | behaviour of `src/core/{Config,Log}.cpp` |
| Mesh camera keys | synthetic `MeshKeys` states fed to `render::applyMeshKeys()` | orbit direction and cancellation, the pitch clamp after hundreds of frames, Q/E as an exact inverse pair, pan direction at yaw 0 and yaw pi/2, target mirrored into the camera - the mapping behind the on-screen camera, testable without a window |

Rules followed when writing these: assert observable behaviour (returned bytes, chosen part, emitted log text), never
that a helper was called; no test reads `/mnt/...`; nothing sleeps or waits on wall-clock time.

## 3. Real-asset verification (manual, read-only)

These need the install and are therefore *not* part of CTest. They were run here and are recorded in `CHANGELOG.md`:

```sh
G="/mnt/2nd4TB/TombRaiderAnniversary/Tomb Raider Anniversary"
./build/oa-asset "$G" list | wc -l                                   # 4304 records across 27 parts
./build/oa-asset "$G" resolve /tmp/oapaths.txt | head                # name -> hash/size/part/specMask
./build/oa-asset "$G" extract --path 'pc-w\lara.drm' --out /tmp/oascan/lara.drm
./build/oa-asset "$G" drm --path 'pc-w\uigeneral.drm'      # unit directory, payload base, anchor check
./build/oa-asset "$G" drm --hash 187BB04C                  # any record; non-DRM records are refused by version
./build/oa-asset "$G" texture --path 'pc-w\lara.drm' --section 0 --out /tmp/oascan/tex0.png   # BC1 -> PNG
./build/oa-asset "$G" texture --path 'pc-w\lara.drm' --section 9 --out /tmp/oascan/tex9.png   # tag 0x15 mask, RGBA
```

Measured here: **every one of the 4,304 records** was run through `drm` - the reproduction line and the resulting payload
census are in `docs/formats/CONTAINERS.md` §8. All **1,098 version-14 units parse byte-exactly**, including level unit
`8baf540e` (235 sections, 92 verified texture anchors, one directory entry whose bytes the archive never stores) and record
`F2197DE6`, whose pre-payload region is 4 bytes below the block A its header declares. The named units of
`docs/formats/DRM.md` parse as before (lara 1712 sections / 71 anchors, uigeneral 155/25, generalbank 247/1); the remaining
3,206 records are refused by leading dword - audio sample rates such as `44100`, the tags `!WAR` and `Mus!`, text payloads
such as `"949\r"` (`objectlist.txt`) and 74 records starting with a zero dword. A parser change has to keep that unit count
at 1,098 or account for the difference out loud.

Textures (`docs/formats/TEXTURES.md`): all 71 type-5 sections of `lara.drm` satisfy the PCD9 header and mip-chain rule —
section 0 decodes to a recognisable clothing atlas (blue top, gloves, belt buckle), section 9 is an RGBA mask whose RGB is
constant white with the shape carried in alpha, and DXT5 section 7 has 5463 blocks of which only five carry colour bytes.
Note that this tool parses every number as hexadecimal, so `--section 12` selects unit section 18.

Extraction only ever opens files `"rb"`; output goes to a scratch directory outside the install and this repository.
Re-runs must produce byte-identical payloads (`cmp` against a previous extraction) before a parser change is trusted.

## 4. Renderer verification (display path)

Four independent checks, because "it drew something" is not evidence:

* **Clear read-back** — `oa-game --selfCheck=true`, phase one: clears an offscreen `R8G8B8A8_UNORM` image (unorm keeps
  clear floats linear), copies it to host-visible memory and compares every pixel with the clear colour within one byte.
  Exercises device creation, command recording, layout transitions and buffer read-back without what is on screen.
* **Sampled-texture round trip** — phase two of the same run: a synthetic 16x16 `B8G8R8A8_SRGB` pattern goes through
  `uploadTexture()` (staging copy per mip region, layout transition, combined-image-sampler descriptor), is drawn with the
  swapchain pipeline into an offscreen attachment that is render-pass-compatible with it, and returns byte-for-byte within
  one unit. The attachment format must equal the pipeline's colour format; the probe also pins UV orientation, because
  row 0 of a payload has to come back as row 0 of the attachment.
* **Presented pixels** — grab the desktop (`spectacle -b -n -o out.png`, see `docs/ENVIRONMENT.md` §"Display"), then
  *measure*: find the largest solid rectangle of the expected colour instead of a loose bounding box (other UI elements
  share the hue). For a 1600x900 window this measured exactly 1600x893 px at (1120, 649) — a few rows under the panel.
  Remember an sRGB attachment encodes clear values: linear `(0.10,0.35,0.62)` arrives as `#59A0CF`.
* **Shipped texture on screen** — `oa-game --gameDir="<install>" --texture='pc-w\lara.drm' --selfCheck=false` uploads the
  record's first PCD9 section (`--section N`, hexadecimal, selects another) and draws it letterboxed. In a 2240x1260
  window the content measured from x=489 across the full drawable height - a centred 1240 px square with clear-colour bars
  at both sides - and matches `oa-asset texture --path 'pc-w\lara.drm' --section 0` texel for texel in layout, which is
  the cross-check between the CPU decode and the GPU upload path.
* **Shipped geometry on screen** — `oa-game --gameDir="<install>" --mesh=8baf540e --selfCheck=true` draws level unit
  `8baf540e` section 0 (149,077 vertex records) as an unindexed run. Two measurements come out of one run: the log line
  `drawing section 0 … 149077 vertices, 49692 triangles, bounds 55103x55225x50366 world units`, and the offscreen probe
  `mesh probe: 6768/9216 pixels covered by 12829 distinct vertex colours, 0 unexplained` — every lit pixel equal to one of
  the shipped colour bytes within an sRGB step. A desktop capture of a 900x600 window on this Wayland session shows the
  level silhouette with facet colours and depth occlusion, but it cannot serve as the byte check: KDE's compositor remaps
  window pixels, so not one of the ~540k window pixels equalled a shipped byte exactly here. Capture for shape, probe for
  bytes (`docs/formats/GEOMETRY.md` §6).
* **Indexed terrain strips** — `oa-game --gameDir="<install>" --mesh=8baf540e --selfCheck=true --strips=true` walks the unit's
  nine oct-trees and uploads the strip index lists their leaves reach, instead of reading the vertex array three records at a
  time: log line `9 terrain tree(s): 1620 leaf reference(s) reached 4776 record(s), drawing 135914 triangle(s) over 149077
  vertex/vertices; 0 bad head(s), 109 broken link(s), 1511 link(s) stepped onto their record`, probe `mesh probe: 2194/9216
  pixels covered by 12829 distinct vertex colours, 0 unexplained`. Chains are entered only from leaf references - never by
  scanning payloads for header-shaped bytes - which is why every head resolves and the counts repeat exactly
  (`docs/formats/GEOMETRY.md` §15); what the stepped links mean is measured in §16. Nothing culls yet, so whole chains draw.
  `oa-asset <install> terrain --hash 8BAF540E` prints the same walk per tree - nodes, leaves, chain health and edge
  percentiles - and refuses to call a payload an oct-tree when its node fields say otherwise.
* **A body over that geometry** — `oa-game --gameDir="<install>" --mesh=8baf540e --strips=true --avatar=true
  --selfCheck=true --frames=1` runs the movement core (`src/game/Avatar.*`) with our own tuning - the log prints it: walk 400,
  run 750, gravity -2000, jump 620, step 25 - as a fixed script of 300 ticks at 1/60 s walking east over the gated index lists
  with one jump. It reports coverage first (`avatar census: 106 of 2,304 sampled column(s) supported across 31,091x5,509
  units; densest spot scores 8/8 at (-4,913.4, 9,497.9) ground 11,948.0`), then the run (`229 supported, 71 airborne,
  1 jump(s), apex 92.2 above spawn ground 11,953.0, path length 2,000.0 units, 17 level change(s)`). The census is the honest
  part: those chains address one compact block of the vertex array (`docs/formats/GEOMETRY.md` §16, §19), so the spawn search
  picks the densest spot and every height stays decode-level - said at Warn in the same log. Five `Avatar.*` tests pin the core
  with no game data: walking at the tuned speed, falling into a pit and landing on its floor, one jump crossing a hole,
  climbing a step it can reach and never lifting onto a taller one, spawn over a hole refused plus leaving-the-world stopping
  the run (`oa_tests --gtest_filter=Avatar*`). The older `--walk=true` probe is gone: fixed to a straight line from the mesh
  centre, it starts over a hole on the gated list and exits with an error while saying less (§19).
* **Driving that body** — add `--avatarDrive=true`, plus `--avatarFollow=true` if you want the look-at to stay on her (the pan
  is an offset from the framing centre, so following a body needs that subtraction). W/S walk, A/D sidestep along the camera's
  right basis, left shift runs, Space jumps; arrows orbit and Q/E dolly as before. Ticks come from a wall-clock accumulator at
  fixed 1/60 s: presentation runs ~158 fps here, so one tick per frame measured 156 ticks/s - 2.6x the tuning's rate. With
  pacing on, consecutive `driven body at tick N` reports land exactly 2.0 s apart and a held sidestep measures ~400 units per
  report window at walk speed. Walking east off the covered patch drops her through a hole; the app then stops moving her with
  a Warn naming it, which is §16-§17's coverage limit rather than a physics failure. `--meshDump=<path>` writes an offscreen
  BMP from either vantage (`openanniversary-work/wmem/follow.png` shows the strip patch at her spawn).
* **Crossing a hole on that geometry** — `oa-game --gameDir="<install>" --mesh=8baf540e --strips=true --avatar=true
  --avatarTraversal=true --vk.validation=false --frames=1` asks whether one take-off clears a hole the shipped strips actually
  leave. It scans the census lattice along eight directions, probing every eighth of a cell: a candidate needs flat ground for a
  full cell behind it (a body on an island has nothing to build speed along), a void at least half a cell wide within jump range,
  and a far side that stays level for another cell and a half. Flat means within three units of the reference height per probe:
  a shore inside a one-stride tolerance can still sag 18 units per tick at run speed - a ~27 degree ramp that falls away faster
  than gravity brings the body down, which is how every attempt failed before the tolerance was tightened. Both ends of the foot
  probe are set by hand because `TerrainQuery::surfaceAt` accepts any surface at or below `fromY + above`: cap one stride over
  the reference and refuse anything more than a stride under it, otherwise a ceiling 1,600 units up answers as ground. The run
  then walks her at run speed and jumps only when the probe a quarter cell ahead stops answering - the rule a player uses
  looking at an edge - and stops the moment she is standing past the far edge, because further along these fragments there is
  more void and a later fall would say nothing about this hole. On `8baf540e`: a 196-unit void from 49 to 246 ahead of spawn, one
  take-off, 35 airborne ticks, standing at ground 11,955.0 having left 11,952.6. Over the sixteen largest version-14 level units
  (`oa-asset <install> list` sorted by size, filtered to v14 units with a general section above 800 kB): ten crossings, six
  reporting no candidate rather than inventing one. Two contact rules keep her on that ground and both fail when removed:
  `Avatar.walksDownASmallStepWithoutLeavingTheGround` (a grounded body sticks to surfaces within one stride below its feet) and
  `Avatar.landsOnAFloorCrossedMidFall` (the foot query anchors on the higher of old and new feet, so a fall longer than one
  stride still meets the floor it crossed) - `oa_tests --gtest_filter=Avatar*`.
* **Walking the level surface** — `oa-game --gameDir="<install>" --level=ma1 --strips=true --avatar=true --avatarWalk=30 --selfCheck=false --frames=1` hands the body a walker instead of the straight-line probe: it keeps its heading while the next probe point holds ground, turns at the first dead end to any direction that holds, and jumps only when no direction does — the same physics and terrain queries the keyboard drives, scripted so a headless run can report it. `pc-w\ma1.drm` (the game's own first level, `docs/formats/CONTAINERS.md` §6.1) measured 1,777.3 units over 30 s with 4 turns before the body left the decoded surface; `pc-w\cn3.drm` held 873.3 units over 20 s. That is continuous supported traversal over the level's own surface; it is not a level end — no end trigger is evidenced anywhere in the unit data (cn3's type-7 records are glyph atlases, GEOMETRY §7) — and the walk instrument says nothing about the keyboard path, which stays the human one above.
* **The audio pipeline** — `--sound=true` runs the engine's own block decoder (`src/cdc/Sound.*`, its tables read straight from `tra.exe`, `docs/formats/AUDIO.md` §3) over 200 copies of the module's fixture block, checks the first four samples against the hand-worked values (same numbers `oa_tests --gtest_filter=Sound*` asserts) and hands 12,800 samples to the platform's audio device through `src/platform/Audio.*` — the device reports its own name and format (here `Ryzen HD Audio Controller` at 48,000 Hz x 2) while our stream is mono S16 at 44,100 Hz, and the fixture's rate is our own choice, not the engine's. `--wavOut=<path>` writes the same PCM as a 25,600-byte canonical WAV next to it: a byte instrument, comparable with `cmp` against any decode of the fixture arithmetic. What this proves is the pipeline and the decoder — shipped sound records keep their framing open (`AUDIO.md` §4-§6: family-1 ≠ block ADPCM, the `.mul` chunk path's byte framing unresolved), so no shipped record has played yet.

* **Pages bound at draw time** — add `--textures=true` to a strips run: the gated triangles group by `matIdx`, each material's class-32 `+12` resolves to one of the unit's type-5 sections **by its `id` field** (not the directory index — an index lookup resolves none, recorded in `docs/formats/GEOMETRY.md` §23), and each material's draw binds its own decoded `PCD9` page; unresolved materials bind a white 1x1 and stay colour-only. Over `4956BEA0` the log prints `136 material(s) grouped into 136 batch(es) over 31 distinct PCD9 page(s) from named sections` with three Warns naming id 9,722 (the §21 unresolved cluster); `--meshDump=<work-dir>/cap/oa_4956BEA0_tex.bmp` writes the picture, and against the shipped start-room capture (`<work-dir>/cap/level.png`) the drawn hall shows the same surfaces — block-moss walls, ivy from ceiling edges, planted greenery — each page on the geometry the shipped game puts it on (`docs/formats/GEOMETRY.md` §23). The colour-only path is untouched: `--selfCheck=true` without `--textures` still answers `0 unexplained`.
* **Query and chain tests** — the query has five data-free tests with exact expected heights: highest surface under foot, top
  floor of two stacked planes, a wall giving no support, refused inputs, cell size taken from the mesh
  (`oa_tests --gtest_filter=TerrainQuery*`). Seven `TerrainStrips.*` tests pin the chain walk on synthetic payloads: a leaf
  head walking to the end of its chain, a bad head counted instead of guessed, a shared tail emitted once, an index naming a
  vertex the unit does not have, a link stepped forward only onto a real header (and one too far ending the chain), a cycle
  walked once, and a payload holding something else (`oa_tests --gtest_filter=TerrainStrips*`).

Validation-layer runs must be clean apart from the host's stale `liblsfg-vk-layer.so` loader messages documented in
`docs/ENVIRONMENT.md`; anything else is a bug to fix, not to suppress.

## 4b. Modernization toggles (settings path)

Every modern feature is a flag that leaves the baseline untouched when absent, and each claim here is a measured number from a real run on this machine (KDE/Wayland, compositor scale 1.4, AMD Radeon RX 7900 XTX):

* **HiDPI is real here** — every window line now prints both sizes: `window '…': logical 1600x900, device 2240x1260, pixel density 1.4`. The renderer works in device pixels end to end (swapchain `2240x1260`), so a scaled display renders sharper rather than resampled — the 1.4 factor comes from this desktop's compositor, and both numbers being logged is what makes the fact checkable on any machine.
* **Ultrawide** — `--width=2560 --height=1080 --selfCheck=true --frames=1` on `pc-w\ma1.drm`: the window reports `logical 2560x1080, device 3584x1512, pixel density 1.4`, the swapchain follows at `3584x1512`, and both baseline probes plus the mesh probe (`3123/9216 pixels covered by 1775 distinct vertex colours, 0 unexplained`) still pass at the wider device size — the same geometry path, no aspect assumptions in it.
* **Presentation mode** — `--present=1` asks the swapchain for the no-wait present instead of the default wait-for-vertical-blend one (with a Warn when a surface does not offer the requested mode and a fallback to the default). Two paced runs, ten real seconds each, `--level=ma1 --avatarDrive=true`: default mode presented `201 frame(s) … (20.2 fps)`; `--present=1` presented `90446 frame(s) … (9159.1 fps)`. The gameplay clock did not follow the picture: both logs report `driven body at tick 120/240/360/480/…` at the same ~60 Hz wall-clock pace — frame-rate independence of gameplay, measured at a 450x presentation spread.
* **Gamepad** — `--gamepad=true` adopts the first connected pad and logs its identity (name/vendor); the platform's `keyDown` then answers from keyboard OR pad (left stick and d-pad → the named direction keys and the WASD aliases, South jumps, East runs, shoulder buttons dolly). With the attached 8BitDo Ultimate powered off it reports honestly — `no gamepad: the platform reports no gamepad` — and the run continues with the keyboard baseline intact. The real-device clause of the contract is still open [to-verify-on-device]: power the pad on and run `oa-game --gamepad=true --avatarDrive=true`, then check the log for `gamepad '8BitDo Ultimate Wireless Controller' vendor … adopted` and compare a held left stick's `driven body at tick N` reports to the keyboard's. Recipe in `docs/ENVIRONMENT.md` §Input devices.

## 5. Static analysis and warnings

All targets compile with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (`-Wno-missing-field-initializers` only, for
Vulkan structs filled field-by-field; justified in `CMakeLists.txt`). Warnings are treated as defects: the two real bugs
found during Milestone 2 (an unused swapchain reference after a semaphore fix, and a `void` SDL call used in a boolean)
were both caught by that set before any test ran. `clang-tidy` is not configured yet — see §6.

## 6. Not covered yet (open gaps, stated plainly)

* **CI is green on GitHub Actions.** `.github/workflows/native.yml` runs the same `scripts/ci.sh` on `ubuntu-24.04`;
  the first run that came back **success** was 39980165579 (16m39s, on commit 53f27a0). Two runner-only facts are
  documented in the workflow's header: ubuntu-24.04's apt mirror has no `glslang` package (so `find_program(OA_GLSLANG
  glslangValidator)` is OPTIONAL and the prebuilt SPIR-V in `shaders/spirv/` is copied at configure time by
  `file(COPY ...)`), and SDL3's `find_package(CONFIG)` needs `CMAKE_PREFIX_PATH="$HOME/sdl3"` to point at the pinned
  `release-3.2.0` install. On that runner the renderer smoke legitimately **skips with exit 77** (SDL_Vulkan_CreateSurface
  says "the specified window isn't a Vulkan window" — xvfb without DRI3), which the workflow treats as success with an
  annotation; the 67-test unit suite and the ASan+UBSan unit suite both run green on the runner regardless. Locally on
  this Wayland host the whole pipeline passes end to end including the smoke drawing shipped content.
* No tests exist for `src/render/vk` beyond the in-binary self-check, and none for `docs/formats/DRM.md` structures —
  those land with the DRM parser (Roadmap Milestone 3 remainder). When they do, the golden constants come from
  `lara.drm`'s section table so the test stays data-free.
* No GPU-independent renderer test on software Vulkan (`lavapipe`) yet; add it when CI runs, because a driver-less runner
  otherwise skips the smoke step silently (exit 77).
* No ASan/UBSan run over the *asset* path with real files; add one together with the DRM parser fuzz entry.
