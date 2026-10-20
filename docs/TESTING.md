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
   open path when `OA_GAME_DIR` is supplied. Skips with exit 77 if `xvfb-run` or a Vulkan ICD is unavailable, so a
   headless machine without drivers cannot report a false pass.
3. Second configure with `-DOA_SANITIZE=ON` (ASan+UBSan) → build → **unit tests only**. Driver/SDL allocations are known
   to leak inside vendor code, so sanitizers guard the parser/core layer, not the display path.

Verified on this host (CachyOS, GCC 16.2.1): plain suite 12/12; the renderer smoke passes against the real GPU through the
Wayland session ("self-check passed", 3 frames presented); ASan+UBSan suite 12/12. The same step under Xvfb without DRI3
exits 77 as a documented skip ("no presentation-capable Vulkan device") instead of failing, since that is an environment
gap rather than a regression. Runner-side behaviour (Ubuntu image plus `mesa-vulkan-drivers`/lavapipe) is *not* verified
yet — see §6.

## 2. Unit tests (`oa_tests`, GoogleTest via CTest)

```sh
cmake -S . -B build -G Ninja -DOA_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build --output-on-failure          # 12 tests, ~10 ms total
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

Validation-layer runs must be clean apart from the host's stale `liblsfg-vk-layer.so` loader messages documented in
`docs/ENVIRONMENT.md`; anything else is a bug to fix, not to suppress.

## 5. Static analysis and warnings

All targets compile with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (`-Wno-missing-field-initializers` only, for
Vulkan structs filled field-by-field; justified in `CMakeLists.txt`). Warnings are treated as defects: the two real bugs
found during Milestone 2 (an unused swapchain reference after a semaphore fix, and a `void` SDL call used in a boolean)
were both caught by that set before any test ran. `clang-tidy` is not configured yet — see §6.

## 6. Not covered yet (open gaps, stated plainly)

* **CI has never executed.** `.github/workflows/native.yml` mirrors the verified local pipeline but no git remote exists
  for this repository, so GitHub Actions has not run it; the runner package set (SDL3 built from a pinned tag, Ubuntu's
  Vulkan stack) is unverified [INFERENCE until first green run].
* No tests exist for `src/render/vk` beyond the in-binary self-check, and none for `docs/formats/DRM.md` structures —
  those land with the DRM parser (Roadmap Milestone 3 remainder). When they do, the golden constants come from
  `lara.drm`'s section table so the test stays data-free.
* No GPU-independent renderer test on software Vulkan (`lavapipe`) yet; add it when CI runs, because a driver-less runner
  otherwise skips the smoke step silently (exit 77).
* No ASan/UBSan run over the *asset* path with real files; add one together with the DRM parser fuzz entry.
