# Roadmap and current status

Acceptance criteria come from the project goal document (`../goal.txt` §13). Status is only `done` when the listed
acceptance evidence exists in this repository; otherwise it says what is missing.

| # | Milestone | Status | Evidence / next step |
| --- | --- | --- | --- |
| 1 | Environment and research | **done** | System audit (`docs/ENVIRONMENT.md`, including the display/capture recipe and the C++-standard status check), game install located read-only, container format documented with addresses (`docs/formats/CONTAINERS.md`), `tra.exe` PE header/import inventory plus prior-art survey with license boundaries (`docs/RESEARCH.md`), six-project audit (`docs/EXISTING_PROJECT_AUDIT.md`), architecture recorded (`docs/ARCHITECTURE.md`). |
| 2 | Buildable native foundation | **done** | `oa-game` builds and runs natively on CachyOS/Wayland with no Wine or Proton: SDL3 window, Vulkan instance with the validation layer active, device selection (AMD RX 7900 XTX), swapchain (`B8G8R8A8_SRGB`, FIFO, 4 images), a clear pass presented 600 times at ~158 fps, and an offscreen read-back self-check that matches the clear colour. The captured desktop shows the window filled with exactly that colour (see `CHANGELOG.md` for the reproduction command). Repeatable test path: `scripts/ci.sh` (unit tests + renderer smoke + ASan/UBSan suite), mirrored by `.github/workflows/native.yml`; procedures in `docs/TESTING.md`. |
| 3 | Asset investigation and tooling | **partly done** | `oa-asset` lists/resolves/extracts records, and `oa-asset drm` dumps a unit's section directory, payload base and any single payload. Container parser validated without game data and against the real install (948 of 949 object-list units resolved). DRM v14 unit parser walks byte-exactly: relocation entries are 8-byte offset/value pairs counted by bits 8-31 **except** for the trailing size-0 type-32 entry, which carries none - that rule alone fixed the two units that previously refused to close (`docs/formats/DRM.md` §2.1); payload base comes from byte-exact closure and every `PCD9` anchor is verified (six named units plus 35 sampled records). Missing: why the extra region before the payloads has the size it does, texture/geometry payload semantics (`PCD9`/DXT1 decode, type-7 layout) and section types other than 5. |
| 4 | First game content | not started | The unit parser already reaches `lara.drm`'s 71 texture and 851 type-7 sections; next is `PCD9`/DXT1 decoding into a Vulkan image plus the first vertex pipeline, then the level coordinate convention. |
| 5 | Playable mechanics | not started | |
| 6 | Functional game loop | not started | |
| 7 | Modernization and usability | not started | |
| 8 | Distribution and maintenance | not started | |

## Open technical questions (tracked here so they do not get lost)

1. DRM v14 unit header: the leading `u32 14` is confirmed; the following counts/section table with `0xFFFFFFFF`
   terminators is not decoded. Yura's `DrmFile` (GPL-3.0, version 14 only) can be consulted for structure but its
   code cannot be copied into this GPL-3.0-or-later tree without matching terms — decide licensing before reading it
   in detail.
2. The five language-flavoured `specMask` groups (≈333 records each) are unconfirmed: identify by extracting one of
   each and comparing content, not by mask arithmetic.
3. ~3,350 archive records still have no name candidate; textures (`.raw`), sound (`.mul`) and movies (`.bik`/`.sch`)
   need path templates recovered from `tra.exe` string xrefs.
4. Licensing: this tree is GPL-3.0-or-later to stay compatible with the sister projects; revisit before any code is
   taken from another project's tree.
