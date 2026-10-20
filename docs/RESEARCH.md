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
| `tr7/cdc/runtime/cdcSound/win32/deADPCM.cpp` | engine ADPCM codec — candidate decoder for sound (type 6) sections |
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
| `DllCharacteristics` | `0x0000` | no ASLR/DEP/CFG; matters only if the original were ever instrumented (we do not run it) |
| Data directories | import `0x85f000`, resource `0x85b000`; exports/exception/bound-import/delay-load empty | no exported symbols to harvest; engine symbols come from disassembly [V-exe] |
| Imported DLLs | `advapi32, binkw32, comctl32, gdi32, kernel32, ole32, oleaut32, shell32, user32, winmm` | **no d3d9/dinput/xaudio import** → Direct3D and input are resolved dynamically; Bink video via `binkw32.dll`; audio through `winmm` plus the engine's own ADPCM [S] §2 |
| Timestamp | `1969-12-31 19:00:00` (zeroed build stamp) | unusable for dating the build |

Corollary: the install also carries third-party `d3d9.dll`/`dxwrapper.dll` shims (`docs/ENVIRONMENT.md`). This project
reads none of them; our renderer talks Vulkan directly.

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
