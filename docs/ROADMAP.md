# Roadmap and current status

Acceptance criteria come from the project goal document (`../goal.txt` §13). Status is only `done` when the listed
acceptance evidence exists in this repository; otherwise it says what is missing.

| # | Milestone | Status | Evidence / next step |
| --- | --- | --- | --- |
| 1 | Environment and research | **in progress** | Done: system audit (`docs/ENVIRONMENT.md`), game install located read-only, container format documented with addresses (`docs/formats/CONTAINERS.md`), toolchain verified. Open: six-project audit (`docs/EXISTING_PROJECT_AUDIT.md`) and architecture decision record. |
| 2 | Buildable native foundation | **not started** | CMake + library/tool/test targets exist, but no window, logging, configuration or Vulkan yet. Acceptance needs a native executable showing a real Vulkan-rendered frame with no Wine/Proton. Next: SDL3 window, device enumeration, one clear-colour swapchain image presented. |
| 3 | Asset investigation and tooling | **partly done** | `oa-asset` lists/resolves/extracts records of the shipped archive; container parser validated by unit tests without game data and against the real install (948 of 949 object-list units resolved). Missing: a second asset type with its own parser + tests (DRM v14 section table is the next target). |
| 4 | First game content | not started | Needs DRM v14 unit parsing (geometry/material sections) and M2. |
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
