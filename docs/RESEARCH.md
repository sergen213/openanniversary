# Prior art and binary inventory (verified)

Checked 2026-10-19/20. Tags: **[S]** secondary source, **[V-exe]** our own disassembly of `tra.exe`, **[V-data]**
reproduced against the shipped install, **[I]** inference. Nothing below was copied into this repository; check the
license note before reading any external source file closely.

## 1. Why this work is not duplicated

No public tool reads Anniversary's DRM v14 units:

* `cdcEngineTools` (Gh0stBlade) targets Underworld/TR8 (DRM v19 + CDRM). A 2019 request for Anniversary extraction was
  pointed there, reported as "none of the releases extracts any useful files", and the thread died unresolved. [S]
  <https://github.com/Gh0stBlade/cdcEngineTools>, <https://www.zenhax.com/viewtopic.php@t=11488.html>
* `gibbed.tombraider.drmedit`: same thread describes it as hard to obtain and unable to load TRA DRMs. [S]
* `Ekey/CDCE.TIGER.Tool` dumps DRM resources through TIGER archives; the PC Anniversary install ships plain
  `bigfile.NNN`, so relevance is structural only. [S] <https://github.com/Ekey/CDCE.TIGER.Tool>
* "TRLAU - Modding Tools" is a closed-source Noesis plugin that views Legend/Anniversary/Underworld/Guardian of Light
  models and textures; no format documentation published. [S] <https://tombraidermodding.com/tool/trulau-mod-tools>

The container layer here (`docs/formats/CONTAINERS.md`) plus the v14 section measurements (`docs/formats/DRM.md`) are
therefore new work, not a re-implementation of something available.

## 2. Closest structural reference: `TheIndra55/cdcEngine` (TR7 decompilation) [S]

<https://github.com/TheIndra55/cdcEngine> — "Decompilation of cdcEngine for Tomb Raider: Legend", C++, **GPL-3.0**. Its
subsystem names are the vocabulary our layers must speak:

| Path there | Use here |
| --- | --- |
| `tr7/cdc/runtime/cdcFile/{FileSystem.h,FileReceivers.cpp}` | receiver chain reading bigfile records; matches the `ResolveReceiver_*`/`ResolveDecompressor_ParseCdrm` names from Underworld's binary [S] and our container disassembly [V-exe] |
| `tr7/cdc/runtime/cdcRender/pc/shared/{PCShaderTable,PCVertexFormat,PCTexture,PCStaticVertexBuffer,…}.h/cpp` | render resources a DRM section deserialises into; naming for vertex formats/shader tables when geometry lands in `src/render` |
| `tr7/cdc/runtime/cdcSound/win32/deADPCM.cpp` | engine ADPCM codec; `tra.exe`'s own path agrees (`FUN_005eca10`: byte count / `0x24` blocks, output `blocks * 0x80`, i.e. 36-byte blocks in and 64 samples out), but which archive bytes feed it is still open - the measured stream-header fields, the encodings ruled out and the two evidence paths that would settle it are in `docs/formats/AUDIO.md` |
| `tr7/cdc/runtime/cdcMath/*`, `cdcScene/Source/{Entity,SceneCellGroup}` | scene/entity model for Milestone 5 semantics |
| `tr7/game/{terrain.h}`, `tr7/game/pc/D3D/d3dterrain.cpp` | the Anniversary terrain vocabulary - 20-byte `TerrainRenderVertex`, octree nodes, per-material strips drawn with i16 index lists plus a base-vertex offset; used to name and cross-check what §9 of `docs/formats/GEOMETRY.md` measured in our own bytes (clone kept at `openanniversary-work/priorart/cdcEngine`) |

License handling: theirs is GPL-3.0-only, ours GPL-3.0-or-later → compatible; any copied line must be attributed and stay
GPL. To avoid a derivative-work claim on our own code, byte-level facts keep coming from our disassembly and our data
measurements; external projects inform naming, structure and cross-version differences only (same rule as
`docs/EXISTING_PROJECT_AUDIT.md`).

## 3. Sibling local reference (already audited)

`openunderworld` (`/mnt/1st4TB/Projects/openunderworld`, GPLv3) documents DRM **v19** in `docs/formats/CONTAINERS.md` §4
with the same 20-byte section header we measured for v14, and records the TR7-vs-TR8 relocation-unit difference
(`count×8` vs bytes), citing cdcEngineTools. Its Legend/Anniversary part alignment (`0x9600000`) was independently
confirmed here (`docs/formats/CONTAINERS.md` §3). Full audit: `docs/EXISTING_PROJECT_AUDIT.md`.

### 3.1 Underworld install byte inventory **[V-data]**

Reproduce: `python3` walk of `/mnt/1st4TB/Tomb Raider Underworld` (`os.walk`, sizes only, read-only). Measured 2026-10-20:
**37 files / 7.32 GiB**, top contributors: `bigfile.000` + `patch.000` = 2,065.9 MiB, `bigfile.001` 2,040.8 MiB,
`bigfile.002` 2,046.9 MiB, `bigfile.003` 817.7 MiB, five `.bik` movies 505.8 MiB, four `.dll` 10.4 MiB, two `.exe`
7.4 MiB, six `.rtf` manuals 3.3 MiB. Two engine-relevant differences from the Anniversary install visible straight from
the file list: Underworld ships `fmodex.dll` (FMOD audio) where Anniversary has no FMOD and imports only `winmm`, and it
splits data across a separate `patch.000` archive. Content-level record inventories for that install already exist in the
sibling project (`openunderworld/docs/formats/INVENTORY.md`: 1,056 CDRM-wrapped DRMs etc.) — reused here rather than
re-derived; only the file-tree statistics above are our own measurement.

## 4. Section magic across engine generations (drives parser design)

ZenHAX thread [S]: Anniversary payloads contain `50 43 44 39 15 00 00 00` ("PCD" + version 21) where Legend uses
`…18 00 00 00` (version 24); the author's unresolved conclusion was that Anniversary meshes look closer to Underworld's
than Legend's.

Ours [V-data]: in `pc-w\lara.drm` every type-5 section starts with ASCII `PCD9`, and the first such block at file offset
`0x92b8` continues with `44 58 54 31` ("DXT1") — the magic family carries **texture** payloads, not only meshes.
`tra.exe` agrees on textures: `FUN_0060c430` compares the first dword with `0x39444350`, fatal-errors with "Corrupt texture
data passed into…", then reads width at +0x10, height at +0x12 and the mip count as byte +0x15 plus one [V-exe]. The dword
after the magic is a per-payload format tag — `'DXT1'`, `'DXT5'` or `0x15` across the 71 texture sections of `lara.drm` — so
ZenHAX's "version 21" bytes are that tag, not a file version [V-data] (`docs/formats/TEXTURES.md`). Mesh-section numbering
stays open (`docs/formats/DRM.md` §4).

## 5. `tra.exe` binary inventory **[V-data]**

Reproduce: `objdump -p "…/Tomb Raider Anniversary/tra.exe"`; imports via
`objdump -x tra.exe | awk '/DLL Name/{print $3}' | sort`. sha256 pinned in `docs/ENVIRONMENT.md`.

| Field | Value | Consequence here |
| --- | --- | --- |
| Format | `pei-i386`, PE32 (32-bit) | the shipped exe cannot run natively on x86-64 Linux without a 32-bit Wine prefix — reinforces native-only reimplementation |
| Image base / SizeOfImage | `0x400000` / `0x860000` (~8.6 MB) | matches Ghidra's default base; documented addresses stay comparable |
| Entry point | RVA `0x238ce8` | — |
| Linker version | 7.0 (MSVC .NET 2003-era) | period style: fixed structs, little C++ exception use in hot paths |
| Characteristics | `0x10f`: relocs stripped, symbols/lines stripped | **no base relocations** → image must load at `0x400000`; our Ghidra project pins that base so address-keyed notes stay stable |
| `DllCharacteristics` | `0x0000` | no ASLR/DEP/CFG; only relevant when instrumenting the original, which happens in a private Wine prefix (`docs/ENVIRONMENT.md`, "Running the original for observation") |
| Data directories | import `0x85f000`, resource `0x85b000`; exports/exception/bound-import/delay-load empty | no exported symbols to harvest; engine symbols come from disassembly [V-exe] |
| Imported DLLs | `advapi32, binkw32, comctl32, gdi32, kernel32, ole32, oleaut32, shell32, user32, winmm` | **no d3d9/dinput/xaudio import** → Direct3D and input are resolved dynamically; Bink video via `binkw32.dll`; audio through `winmm` plus the engine's own ADPCM [S] §2 |
| Timestamp | `1969-12-31 19:00:00` (zeroed build stamp) | unusable for dating the build |

Corollary: the install also carries third-party `d3d9.dll`/`dxwrapper.dll` shims (`docs/ENVIRONMENT.md`). This project
reads none of them; our renderer talks Vulkan directly.

### 5.1 Chapter selection reaches real loader code, measured [V-exe]

A menu table at `0x6639bc` named `Chapters` holds fifteen entries (`chapter1` … `chapter15`, string addresses
`0x664854`…`0x6649a4`), each paired with the same handler. Reading it out of the image and then decompiling the chain:

| address | what it does [V-exe] |
| --- | --- |
| `FUN_00427a30` | takes the selected entry, checks `1 < index <= count`, calls `FUN_00422090(entry)` with a 400-byte record stride |
| `FUN_00422090` | `stricmp`s the chapter name against the table, then sets current-chapter globals and calls `FUN_00455890(index)`; failure prints `chapter %s doesn't exist` (`0x6505cc`) |
| `FUN_00455890` | level start; its only callee is `FUN_004559f0`, and it is also called from the container-side `FUN_005bf770` |
| `FUN_004559f0` | chapter bookkeeping: picks a per-chapter short out of the record at `DAT_00812c68 + 4`, and makes an indirect call through `PTR_FUN_0066543C` |

That pointer slot (`0x66543C`) stores `FUN_004e8540` in the image - a function whose body is just `return;` - so the real
level-load routine is installed at runtime, and no static write to that slot exists (base-relative addressing again). The
same trick explains why string-based xref hunting fails (`docs/formats/GEOMETRY.md` §17).

A console flag table sits beside it: entries every twelve bytes from `0x664c58` to `0x664d90` name `polylines`, `qstats`,
`collision`, `tcollision`, `signals`, `axis`, `bones`, `constraints`, `radius`, `noterrain`, `normals`, `noweighting`,
`soft`, `markup`, `markers`, `vmarkers`, `pmarkers`, `portals`, `nohud`, `streamsignals`, `timeline`, `texttvsafe`,
`graphictvsafe`, `uimemory`, `dontdisablemenus`, `donthidemenus`, `unlockall` [V-data]. Those are engine debug switches,
and they mark the terrain/collision drawing code as switchable at runtime - useful handles when reading that code.

## 6. Language/toolchain status (goal §2) **[S] + [V-data]**

* Latest *published* C++ IS right now: **C++23**. WG21's last approved working draft for the next revision is N5046
  (2026-05-12); editors' reports N5047/N5055 (2026) state the Draft International Standard had not been completed, and
  P4238R0/R1 (July 2026) was still contesting parts of C++26. <https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/n5046.pdf>,
  <https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/n5047.html>,
  <https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p4238r1>. Re-check before raising the project floor.
* Local compilers accept the next mode experimentally: `g++ -std=c++26` and `clang++ -std=c++26` both compile a trivial
  TU on this host (GCC 16.2.1, Clang 23.1.3) [V-data]. The build stays at `-std=c++23`; move the floor only once C++26 is
  actually published.
* Repo floor: `cmake_minimum_required(VERSION 3.28)`; host carries CMake 4.4.4 and Ninja 1.13.2 (full pinned list in
  `docs/ENVIRONMENT.md`).

## 7. The shipped game's own settings tree **[V-data]**

`wine-tra/user.reg` (the prefix built by `docs/ENVIRONMENT.md`'s run recipe) contains the key the shipped `tra.exe`
writes and reads: `HKCU\Software\Crystal Dynamics\Tomb Raider: Anniversary`, with subsections `\Graphics` and
`\Input`. Writing the keys and launching is what community notes say makes the game skip its start-time setup dialog
(pcgamingwiki.com TR Anniversary page, [S]); under Wine the dialog appears regardless — settled negative 2026-10-22,
see `docs/ENVIRONMENT.md`. What the tree contributes as fact:

* renderer feature vocabulary — `EnableShadows`, `EnableReflection`, `EnableDepthOfField`, `EnableWaterFX`,
  `EnableFullscreenEffects`, `EnableVSync`, `AntiAliasing`, `Contrast`, `Brightness`, `BestTextureFilter`,
  `UseShader30`, `UseShader20`, `CreateGameFourCC`, `AlwaysRenderZPassFirst`, `FullscreenModeID` (packed-mode dword,
  0x300400 on this prefix's last write) — exactly the option set the launcher dialog draws; this is the option list a
  settings screen for Milestone 7 should speak, named by the shipped engine itself, not guessed;
* audio/control plumbing — `MusicVolume`/`SfxVolume`/`VoVolume` as dwords at 0x80, `UseReverb`, `Vibration`,
  `DisableForceFeedback`, `AimXInverted`/`AimYInverted`/`CameraXInverted`/`CameraYInverted`, `MouseSensitivity`,
  `CombatMode`, `Hints`, a zero-filled `Gamepad` string, and an `\Input` subsection of numeric key rows;
* `CreateGameFourCC=1` and the `MRUProfileName`/"Default Profile" pair point at a FourCC-tagged profile blob —
  consistent with the section-magic generations of §4; keep as a lead, not a claim.

Reproduce: `grep -a -A40 'Crystal Dynamics' ../../openanniversary-work/wine-tra/user.reg`. The values here came from an
October session that reached the hall; treat per-value choices as this prefix's state, the *key set* as shipped-engine
fact.

## Movement constants: what the shipped engine actually does (V-exe)

The M5 behavior cross-check asked whether our model's tuning (`walkSpeed 400 / runSpeed 750 / gravity -2000 / jumpSpeed 620 /
maxStepUp 25` world-units-per-second, `src/game/Avatar.hpp`) appears in `tra.exe`. It does not appear *as code immediates* — and
that negative is itself the finding:

- **The player-update code reaches its movement values through accessor wrappers over data**, not literals. The decompiled
  physics-roll accessor (`FUN_00566de0`, the function that stores `s_physicsRollProxy` at `0x6569d0`) reads and writes every
  movement quantity as a struct field: `return (float10)*(float *)(param_1 + 0x148);`, `*(float *)(iVar7 + 0xac) = ...`,
  chains of `FUN_00508xxx()` setter/getter calls keyed by fixed state offsets (`+0xca0/+0xca4/+0xca8`, a list walked with
  `+4` stride). The two direct string→code users the xref probe found (`physicsRollProxy` at `0x6569d0`, `Climber Proxy`
  at `0x656628` — full probe recorded in `openanniversary-work/ghidra/exports/movement-strings-xrefs.tsv`) both take this shape.
- **Float search, honest result.** `750.0f`, `620.0f` and `25.0f` do not occur anywhere in the decompiled corpus (`decomp_all.c`),
  nor do their per-frame forms at 30/60 Hz. `400.0f` occurs 38× and `2000.0f` 15× but in unrelated contexts — camera far,
  `(2000,2000,2000)` clamp triples, `999999.0` sentinels — none is a player-update function, and no function loads both.
  Attributing them to movement would be a numeric coincidence, not evidence.
- **What is pinned [V-exe] exactly once**: the climb proxy. The function that reads `Climber Proxy` computes
  `delta = *(float *)(player + 0x108) - 200.0f` clamped at `0.0` (the `0.0`/epsilon pair decoded live from the `.rsrc` mapping
  `VA 0x401000 ↔ file 0x200`): the shipped game's hand-over reach is 200 world units on the *level's own scale*, matching
  the GEOMETRY §24 claim that designed edges cluster in 25–200-unit bands. Our verified traversal's climb steps stay at or
  below that reach.

**Conclusion for M5's "validated against original-game behavior" half:** the exe proves the *mechanism* — per-tick accessor
field updates over a data-driven state table (gravity as a signed field written at `+0xac`-class offsets, velocity as
accessor chains) — while the *numbers* are bounded [V-data]: our model clears the designed short edges (the 8baf540e envelope's
25–200-unit cluster and the 4956BEA0 396-unit jump) and does not clear the level's p50 1562-unit voids. The model reproducing
that envelope, plus the 200-unit climb reach, is the cross-check; equality of our five tuning literals to shipped floats is
explicitly *not* claimed, and is probably false — the shipped engine keeps them in data tables we have not located by name.

## Movement mechanics — what the shipped engine actually does (V-exe)

The behavior cross-check for the verified traversal (M5) reads `tra.exe` statically; no numbers were inferred from our model:

- **Movement values are state slots, not code literals.** The physics-roll accessor (`FUN_00566de0`, which stores
  `s_physicsRollProxy` at `0x6569d0`) reads/writes every quantity through a getter chain over player-state offsets
  (`return (float10)*(float *)(param_1 + 0x148);`, writes at `+0xac/+0xb0/+0x6c..` walked with stride 4). The engine integrates
  velocity/gravity as fields of the player record; tuning tables live in data the accessors dereference.
- **Climber Proxy reach pinned exactly.** The function referencing `Climber Proxy` (at `0x656628`) computes
  `delta = *(float *)(player + 0x108) - DAT_00654684` and clamps `delta` at `0.0` before storing through
  `FUN_0050e970`; `DAT_00654684` decodes live from the `.rsrc` mapping (`VA 0x401000 ↔ file 0x200`) to exactly **200.0f** —
  the shipped game's hand-over climb budget is 200 world units on the level's own scale, the same 25–200-unit edge band
  GEOMETRY §24 measured from the archive side. Our model's verified traversal climbs stay at/below that reach.
- **Negative, stated honestly:** the literal set `750.0f / 620.0f / 25.0f` (our run/jump/step tuning) occurs **nowhere** in the
  decompiled corpus, and their per-frame forms at 30/60 Hz do not either; `400.0f` (38 sites) and `2000.0f` (15 sites) occur
  but only in unrelated contexts — `(2000,2000,2000)` clamp triples, `999999.0` sentinels, camera far values — no function
  loads both, so none may be attributed to player physics. Our model's constants were chosen to *fit the shipped data's
  envelope* (gap widths, the 200-unit climb budget), not to equal shipped floats, and this section records that distinction
  instead of claiming a numeric match.

Repro: `openanniversary-work/ghidra/exports/movement-strings-xrefs.tsv` (the ExportXrefs string-scan run above), the two
decompiled users in `decomp-player/`, and the float decode against the shipped `tra.exe` bytes via the .rsrc mapping.
