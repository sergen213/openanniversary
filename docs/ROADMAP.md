# Roadmap and current status

Acceptance criteria come from the project goal document (`../goal.txt` §13). Status is only `done` when the listed
acceptance evidence exists in this repository; otherwise it says what is missing.

| # | Milestone | Status | Evidence / next step |
| --- | --- | --- | --- |
| 1 | Environment and research | **done** | System audit (`docs/ENVIRONMENT.md`, including the display/capture recipe and the C++-standard status check), game install located read-only, container format documented with addresses (`docs/formats/CONTAINERS.md`), `tra.exe` PE header/import inventory plus prior-art survey with license boundaries (`docs/RESEARCH.md`), six-project audit (`docs/EXISTING_PROJECT_AUDIT.md`), architecture recorded (`docs/ARCHITECTURE.md`). |
| 2 | Buildable native foundation | **done** | `oa-game` builds and runs natively on CachyOS/Wayland with no Wine or Proton: SDL3 window, Vulkan instance with the validation layer active, device selection (AMD RX 7900 XTX), swapchain (`B8G8R8A8_SRGB`, FIFO, 4 images), a clear pass presented 600 times at ~158 fps, and an offscreen read-back self-check that matches the clear colour. The captured desktop shows the window filled with exactly that colour (see `CHANGELOG.md` for the reproduction command). Repeatable test path: `scripts/ci.sh` (unit tests + renderer smoke + ASan/UBSan suite), mirrored by `.github/workflows/native.yml`; procedures in `docs/TESTING.md`. |
| 3 | Asset investigation and tooling | **partly done** | `oa-asset` lists/resolves/extracts records, `oa-asset drm` dumps a unit's section directory (including entries whose bytes the archive never stores), payload base, decoded relocations and any single payload, and `oa-asset texture` decodes a unit's mip 0 to PNG. Container parser validated without game data and against the real install (948 of 949 object-list units resolved). The DRM v14 parser now walks **all 1,098 version-14 units** in the install byte-exactly with every `PCD9` anchor verified: relocation entries decode to kind / target section / slot plus addend straight from `tra.exe`'s loader (`docs/formats/DRM.md` §2.1), and a payload base is accepted only when it both closes at end-of-image and lands every magic - re-solving it from an actual `PCD9` when a directory entry's bytes are absent, as in level unit `8baf540e`, and when nothing anchors falling back to the declared block A (§3). Census of all 4,304 records: `docs/formats/CONTAINERS.md` §8. Texture payloads documented for every tag seen so far, mip-chain rule checked byte-for-byte on all 71 of `lara.drm`'s (`docs/formats/TEXTURES.md`). Missing next: name candidates for the ~3,350 unlabelled records and readers for the audio families (`!WAR`, `Mus!`, sample-rate headers); geometry sits under M4. |
| 4 | First game content | **partly done** | Textures are measured end to end on real data: all 71 PCD9 sections of `lara.drm` satisfy the documented header and mip-chain rule, section 0 decodes to a recognisable clothing atlas and tag-`0x15` sections decode as RGBA masks (`docs/formats/TEXTURES.md`, commands in `docs/TESTING.md` §3). The GPU path carries shipped bytes: `oa-game --texture='pc-w\lara.drm'` uploads that section's 10-level BC1 chain through `VkRenderer::uploadTexture()` and draws it with premultiplied blending plus aspect-fit letterboxing, a desktop capture during the run matching what the CPU decoder produced. Geometry now goes container to pixels for one case: level unit `8baf540e` section 0 holds exactly 149,077 stride-20 vertex records (i16 x/y/z, constant u16, ABGR colour, i16 UV at 1/4096 [S], zero pad), and `oa-game --mesh=8baf540e` uploads those bytes verbatim, draws the unindexed run depth-tested with an orbit camera, shows it in a window as a level-sized silhouette, and asserts byte-exact coverage offscreen (`docs/formats/GEOMETRY.md` §6). `lara.drm`'s largest general payload is float-bearing instead, and relocations bind sections to each other rather than listing vertices (§2-§4). The shipped topology is now decoded and drawn: strip records gated by index locality and edge scale give 4,091 records whose triangles share **79.8%** of their edges - a connected mesh - where the unindexed run shares none, and `oa-game --mesh=8baf540e --strips=true` uploads them as an index buffer (`docs/formats/GEOMETRY.md` §13). Still missing for acceptance: which texture a `matIdx` selects (92 values measured), how oct-tree leaves pick a sub-run inside these chains (§10), culling - every chain draws whether seen or not - winding order, level coordinates and scene graph. |
| 5 | Playable mechanics | not started | |
| 6 | Functional game loop | not started | |
| 7 | Modernization and usability | not started | |
| 8 | Distribution and maintenance | not started | |

## Open technical questions (tracked here so they do not get lost)

1. Geometry inside units is now a per-content gap, not a container or layout gap. The level oct-tree is decoded from
   shipped bytes - self-relative nodes in one payload, each leaf's +16 slot relocated into one of 34 sibling sections
   (`docs/formats/GEOMETRY.md` §9) - but the earlier reading of those targets as i16 strip runs over the stride-20 vertex
   array (§2) is refuted: indexed against section 0 they sit at 20-64% RMS longest edge, far from coherent. What those
   payloads do hold are `TerrainTextureStripInfo` records - a 44-byte header plus u16 indices into section 0 - 4,284 of them
   in unit `8baf540e`, chaining through `nextTexture` inside one payload and coherent at ~0.4% (§10). Still open: how a leaf
   reaches those records (only 85 of 1,620 addends land on a record start), what the other payload kinds are, per-leaf
   material/texture-page selection (section 86 also relocates against a `PCD9` section), why only ~10% of section-0 vertices
   get indexed, and model-unit indexing for non-terrain meshes. Next step is reading `tra.exe`'s terrain loader through the
   per-type handler table (`DAT_0088e2b0 + type*4`, §5/§7) instead of inferring from data alone; `oa_cdc` already walks the
   tree and `oa-asset terrain` prints both measurements.
   Licensing note stands: consulted prior art (cdcEngine tr7, GPL-3.0) is licence-compatible with this GPL-3.0-or-later tree;
   keep attribution if any code is taken.
2. The five language-flavoured `specMask` groups (≈333 records each) are unconfirmed: identify by extracting one of
   each and comparing content, not by mask arithmetic.
3. ~3,350 archive records still have no name candidate; textures (`.raw`), sound (`.mul`) and movies (`.bik`/`.sch`)
   need path templates recovered from `tra.exe` string xrefs.
4. Licensing: this tree is GPL-3.0-or-later to stay compatible with the sister projects; revisit before any code is
   taken from another project's tree.
