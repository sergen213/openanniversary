# OpenAnniversary

A free, original reimplementation of *Tomb Raider: Anniversary* (2007, Crystal Dynamics cdc/Legend engine)
for Linux and Windows, built from the ground up. No game code, no game assets and no proprietary binaries
are part of this repository; you must own a legally obtained copy of the game and point the loader at it.

Status: **pre-alpha**. The container layer (cdc `bigfile` archives) is implemented and verified against the shipped
archive, and a native SDL3 + Vulkan window renders real presented frames on Linux without Wine or Proton. Payload
parsing (DRM v14 units), geometry rendering and gameplay are still ahead. See `CHANGELOG.md`, `docs/ARCHITECTURE.md`
and `docs/ROADMAP.md`.

## Legal posture

* Everything in this repository is original work written from behavioural observation of the user's own copy
  of the game, plus published format documentation. It is licensed under GPL-3.0-or-later (see `LICENSE`).
* Derived reverse-engineering material (decompilations, Ghidra projects, string dumps, extracted assets) lives
  outside the repository in a sibling `*-work/` directory and is git-ignored. Never commit it.
* Asset loading reads the installed game files read-only. Nothing is written next to them; no DRM, no archive
  rewriting, no network calls.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
ctest --test-dir build --output-on-failure      # unit tests, no game data required
```

Requirements: CMake ≥ 3.28, a C++23 compiler (GCC 16 / Clang 23 verified), zlib, GoogleTest.
Vulkan SDK + SDL3 are added with the rendering layer; see `docs/ENVIRONMENT.md` for the verified toolchain.

## Tools

| Tool | Purpose |
| --- | --- |
| `build/oa-asset` | Inspect and extract a shipped `bigfile.NNN` archive (read-only) |
| `build/oa-game` | Native window + Vulkan frame loop; `--selfCheck=true` proves read-back without a display |

```sh
# list every record of an install, resolve names, extract one unit
oa-asset "<install dir>" list
oa-asset "<install dir>" resolve candidate-paths.txt
oa-asset "<install dir>" extract --path 'pc-w\lara.drm' --out lara.drm
```

## Documentation

| File | Contents |
| --- | --- |
| `docs/ENVIRONMENT.md` | Verified host/toolchain facts the project is developed against |
| `docs/formats/CONTAINERS.md` | cdc bigfile layout, path hashing, specialisation masks, evidence trail |
| `docs/RESEARCH.md` | Prior art and community documentation, with links |
| `docs/EXISTING_PROJECT_AUDIT.md` | What the existing local reimplementation projects already contain |

## Ground rules for contributors

1. No proprietary bytes in git: no `.exe`, `.dll`, `bigfile.*`, `.drm`, `.bik`, `.mul`.
2. Format claims need an address (function or offset) plus a reproducible command, or a citation.
3. Every format parser gets unit tests that run without game data.
4. No speculative abstraction: the ladder in the project notes applies — reuse first, stdlib second, minimum code last.
