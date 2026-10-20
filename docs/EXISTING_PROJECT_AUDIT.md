# Audit of the existing native-port projects on this machine

Method: static reading only (no builds or tests were run, to keep memory free for Ghidra work) — `git log/status`
per project, LOC counts over C/C++ sources excluding build and analysis trees, `add_test()` counts from root
CMakeLists, README/docs headings, source-tree listings. Facts the auditor did not re-measure are marked
**[prior-audit]**, referring to the file-level reuse audit in
`/mnt/1st4TB/Projects/openunderworld/docs/EXISTING_PROJECT_AUDIT.md` (2026-10-18).

Headline: **only OpenUnderworld is on the same engine family** (Crystal Dynamics cdc / Legend engine). The others run
Conitec/Acknex (Kabus 22, Citroën C4 Robot, SüperCan), an ETI Yami custom VM, a Quake3-style BSP engine **[prior-audit]**
(Pusu: Uyanış), a custom MMO client/server (IKV) and Lionhead's Black & White engine via bgfx (openblack). No
`tomb raider|cdcgame|bigfile|legend engine` mention exists in the other five ports' docs or READMEs.

## 1. OpenUnderworld — `/mnt/1st4TB/Projects/openunderworld`

* VCS: HEAD `2bb0e63` ("Release 0.4.0", 2026-10-18), 50 commits, clean tree, remote
  `github.com/sergen213/openunderworld`, tags `v0.1.0…v0.4.0`.
* C++20, CMake+Ninja (≈20 build trees on disk), OpenGL 4.5 backend (`src/render/gl/GlRenderer.cpp`) **and** a Vulkan
  backend (`src/render/vk/VkRenderer.cpp`, 10 SPIR-V shaders), SM3→GLSL/SPIR-V through libshaderc, thin SDL layer
  `src/platform/Sdl.hpp`.
* README claims match the code and understate it: viewer + game apps, animation/blend/hair, collision, rigid-body
  physics with hinge doors, combat/AI, saves, scripts, locale, audio mixer for `.mus`, movies, UI, config, launcher,
  self-update. Commit messages cite original-exe addresses as evidence (CINE header `0x456580`, ShaderLib at
  `tru.exe 0x506980`), so claims are traceable.
* Formats already parsed: cdc bigfile + `patch.000` with CRC-32 path hashes (`src/cdc/Cdc.hpp`), levels, models,
  scenes, textures, shader libraries (`src/formats/{Level,Model,Scene,Texture,Shader}.cpp`, 338/493/263/149/405 lines),
  D3D9 SM3 parse+translate (`src/render/ShaderTranslate.cpp`, 535 lines), CINE cutscenes (`src/cine/Cine.cpp`),
  saves, scripts, text/locale, audio, video. Format docs: `docs/formats/{CONTAINERS,LEVELS,MESHES,ANIMATION,COLLISION,
  PHYSICS,MATERIALS,TEXTURES,SHADERS,SCRIPTS,AUDIO,VIDEO,CINEMATICS,TEXT,INVENTORY}.md`, engine notes in `docs/re/*`.
* License: root `LICENSE` is GPLv3 → copyleft, compatible with this repository.
* Maturity: 93 files ≈ 24.8k lines; 19 `tests/test_*.cpp` plus 9 scripted replays (`tests/replays/*.txt`); tools
  `ou-asset`(431 LOC), `ou-audio`, `ou-movie`, `ou-text`, `ou-imgdiff`, `ou-sign`; AppImage + `PKGBUILD` + `.desktop`
  packaging; release signing key material present.
* Directly relevant to Anniversary: `docs/formats/CONTAINERS.md:43` records the Legend/Anniversary vs Underworld part
  alignment difference (`0x9600000` vs `0x7FF00000`) — independently confirmed here in §3 of our own
  [formats/CONTAINERS.md](formats/CONTAINERS.md); `docs/RESEARCH.md:347` is a TR7/TRAE-vs-TR8 comparison table;
  Yura's `DrmFile` accepts DRM version **14** only (TR7/TRAE, `Yura/Formats/DrmFile.cs:25`).
* Side-car scratch tree `/mnt/1st4TB/Projects/openunderworld-work/{ghidra,manifests,namelists,research}` is the pattern
  our own `openanniversary-work/` follows (Ghidra project + input sha256 pinning + machine-readable inventories).

## 2-7. The other five ports (different engines; reference value only)

| Project | Path | Engine | Tests / size | License | What is Anniversary-relevant |
| --- | --- | --- | --- | --- | --- |
| Kabus 22 | `/mnt/1st4TB/Kabus 22` | Conitec/Acknex A6 | 14 `add_test`, ≈49k lines, 216 files | **none** (`README.md:258`: "No project-wide license has been assigned.") | Ed25519-signed self-update with rollback (`installer/update.*`), AppImage pipeline, dlopen'd Vulkan loader + SPIR-V embedding (`native/vulkan_loader.*`, `embed_spirv.py`), Wine+SSIM original-vs-native visual oracle (`tools/wine-reference/`) |
| etiyami | `/mnt/1st4TB/etiyami` | ETI Yami custom VM | 9 `add_test`, ≈22.5k lines; **36 uncommitted entries** | none | X-macro Vulkan dispatch (`native/vulkan_api.hpp`), renderer pixel-capture self-checks (`native/check_vulkan.cpp`), `.run`/AppImage packagers |
| Pusu: Uyanış | `/mnt/1st4TB/Pusu: Uyanış` | Quake3-style BSP **[prior-audit]** | 2 `add_test` + 10 harnesses in `checks/*.cpp`, ≈29k lines; 15 uncommitted | **GPL-3.0-only** | atomic publish/import ledger (`src/importer.cpp`, `renameat2` publication), display-less CLI smoke pattern (`checks/game_cli.py`), pure-CMake SPIR-V embedding. **Never copy `src/resources.cpp`** (embedded copy-protection key) **[prior-audit]** |
| IKV | `/mnt/2nd4TB/IKV` | custom MMO client/server | 18 `add_test`, ≈38.6k lines, 27 Python tools | none; vendored FidelityFX is MIT | strongest signed update stack here (`src/launcher_updates.cpp`, anti-rollback journal), offscreen Qt GUI smoke test, Vulkan RT + FSR3 wiring, renderer math tests |
| Citroën C4 Robot | `/mnt/2nd4TB/CitroenC4Robot` | Acknex A8 + D3D9 | 26 `test_*` sources (README reports 28 CTest), ≈38.5k lines; **not a git repo** | none | best CI template on the machine (`.github/workflows/native.yml`: pinned actions, SDL3 built from tarball, double-build `cmp` reproducibility), Xvfb real-binary smoke tests (`tests/test_native_window.py`, `test_vulkan_native.py`), Vulkan dispatch + validated pipeline cache, SHA-pinned tool acquisition |
| SüperCan | `/mnt/2nd4TB/SuperCan` | Acknex A8 | 37 `add_test` (highest density), ≈40k lines; **not a git repo** | none | most Anniversary-relevant generic renderer code: D3D9 SM1–3 bytecode parser + CTAB/preshaders (`src/assets/d3d9_shader.cpp`) and D3D9→GLSL lowering incl. depth `z' = 2z − w`, half-pixel offset, winding mirror; hardened read-only importer walk (`openat`/`O_NOFOLLOW` + SHA re-check); `SKIP_RETURN_CODE 77` convention for asset-dependent tests |
| openblack | `/mnt/1st4TB/openblack` | Lionhead Black & White via bgfx | 7 test sources, ≈162k lines (upstream scale) | GPL-3 (upstream's, not ours to relicense) | structural reading only. Current checkout is a self-described non-compiling WIP commit with 13 uncommitted files; there is **no** `src/BigFiles/` here despite the name similarity — nothing in that tree parses TRAE-style archives |

## Reuse decisions for this repository

1. **Take now (GPL-compatible, same author):** OpenUnderworld's format *documents* and its test/packaging conventions
   (replay-driven acceptance tests, `SKIP_RETURN_CODE 77` style gating, AppImage/PKGBUILD layout). Its container code is
   already superseded: our `src/cdc/Bigfile.*` was written from `tra.exe` evidence and needs no port.
2. **Ask the owner (all rights reserved today):** Kabus 22, etiyami, IKV, Citroën C4 Robot, SüperCan have **no license
   grant at all**, so they impose no GPL obligation — they simply grant nothing. Copying any file requires an explicit
   `SPDX-License-Identifier` from the owner plus provenance in the commit. Until then: reference reading only. The two
   highest-value candidates for that grant are SüperCan's D3D9 shader parser/lowering and Citroën C4 Robot's CI + Xvfb
   smoke harness.
3. **GPL-3.0-only (Pusu) and upstream GPL-3 (openblack):** narrower or not ours to relicense; use patterns, not files.
4. **Never copy regardless of license:** original media and installers, all `analysis/`, `reverse/`, `work/*` dumps,
   Ghidra `.rep` databases, embedded protection keys, and any release private key (`packaging/ikv-release-public.pem`
   style artifacts → generate our own).
5. **Tree hygiene:** two of the six ports have no VCS at all (Citroën C4 Robot, SüperCan) and two hold unreleased WIP
   (etiyami 36 dirty, Pusu 15 dirty); any future lift must record exact source file hashes in the commit message.
