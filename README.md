# OpenAnniversary

An original, native Linux reimplementation of *Tomb Raider: Anniversary* (2007,
Crystal Dynamics cdc/Legend engine) that runs against your own installed copy
of the game.

## Status

**Pre-alpha. Not playable.** The container layer (cdc `bigfile` archives) and
the DRM v14 unit parser are implemented and checked against the shipped
archive. A native SDL3 + Vulkan window renders real frames on Linux without
Wine or Proton, including shipped texture mip chains and level terrain decoded
from the game's own bytes, with a simple test body that can walk over it.
Models, materials and gameplay are still ahead. See
[docs/ROADMAP.md](docs/ROADMAP.md) for milestone status and
[CHANGELOG.md](CHANGELOG.md) for details.

## You need the original game

No game code, game assets or proprietary binaries are part of this
repository. You must own a legally obtained copy of the game (developed
against the GOG build) and point the tools at its install directory.
*Tomb Raider* and its content are © their respective owners; this project is
not affiliated with or endorsed by them.

- Everything here is original work, written from behavioural observation of
  the user's own copy of the game plus published format documentation.
- Asset loading reads the installed game files read-only. Nothing is written
  next to them; no DRM, no archive rewriting, no network calls.
- Derived reverse-engineering material (decompilations, Ghidra projects,
  string dumps, extracted assets) lives outside the repository in a sibling
  `*-work/` directory and is git-ignored. Never commit it.

## Download

Tagged releases are on
[GitHub Releases](https://github.com/sergen213/openanniversary/releases). They
are source-only; there are no prebuilt binaries, so build from source.

## Requirements

- Linux on x86_64 with a Wayland or X11 display.
- A Vulkan-capable GPU and driver (Mesa lavapipe works for the smoke test).
- To build: CMake ≥ 3.28, Ninja, a C++23 compiler (GCC 16 / Clang 23
  verified), zlib, GoogleTest, SDL3 and the Vulkan loader + headers (the full
  Vulkan SDK is not required). `glslangValidator` is optional; without it the
  prebuilt SPIR-V in `shaders/spirv/` is used.

## Quick start

```sh
G="<your install dir containing bigfile.000..026>"

# List every record of the install, extract one unit (read-only):
./build/oa-asset "$G" list
./build/oa-asset "$G" extract --path 'pc-w\lara.drm' --out lara.drm

# Render a level's terrain and textures in a window:
./build/oa-game --gameDir="$G" --level=cn3 --strips=true --textures=true

# Walk a test body over the decoded ground (W/S/A/D + Space):
./build/oa-game --gameDir="$G" --level=ma1 --strips=true --avatar=true --avatarDrive=true
```

| Tool | Purpose |
| --- | --- |
| `oa-asset <game-dir> <list\|hash\|resolve\|extract\|drm\|namescan\|texture\|terrain\|terrper>` | Inspect and extract a shipped `bigfile.NNN` archive (read-only) |
| `oa-game` | Native window + Vulkan frame loop; `--selfCheck=true` verifies the frame by read-back |

`oa-game` options use the `--key=value` form. They can also come from an
`openanniversary.ini` in the working directory, or from the file named by
`OA_CONFIG`. More commands are in [docs/BUILDING.md](docs/BUILDING.md) and
[docs/TESTING.md](docs/TESTING.md).

## Build from source

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
ctest --test-dir build --output-on-failure      # unit tests, no game data required
```

[docs/BUILDING.md](docs/BUILDING.md) covers getting SDL3, the sanitizer
build and running against your own install.

## Documentation

- [docs/BUILDING.md](docs/BUILDING.md) — build and run instructions.
- [docs/TESTING.md](docs/TESTING.md) — test pipeline and manual real-data checks.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — code structure and design.
- [docs/ROADMAP.md](docs/ROADMAP.md) — milestones and their status.
- [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) — the host and toolchain the project is developed against.
- [docs/RESEARCH.md](docs/RESEARCH.md) — prior art and community documentation, with links.
- [docs/formats/CONTAINERS.md](docs/formats/CONTAINERS.md) — cdc bigfile layout, path hashing, specialisation masks.
- [docs/formats/DRM.md](docs/formats/DRM.md) — DRM unit format.
- [docs/formats/TEXTURES.md](docs/formats/TEXTURES.md) — texture format.
- [docs/formats/GEOMETRY.md](docs/formats/GEOMETRY.md) — geometry and terrain.
- [docs/formats/AUDIO.md](docs/formats/AUDIO.md) — audio formats.
- [docs/EXISTING_PROJECT_AUDIT.md](docs/EXISTING_PROJECT_AUDIT.md) — audit of related native-port projects.

## Contributing

Changes are accepted only as pull requests to this repository, reviewed and
merged by the maintainer. See [CONTRIBUTING.md](CONTRIBUTING.md). Ground rules:

1. No proprietary bytes in git: no `.exe`, `.dll`, `bigfile.*`, `.drm`, `.bik`, `.mul`.
2. Format claims need an address (function or offset) plus a reproducible command, or a citation.
3. Every format parser gets unit tests that run without game data.
4. No speculative abstraction: reuse first, standard library second, minimum new code last.

## License

Source-available, all rights reserved — see [LICENSE](LICENSE). Contributions
via pull request only (see [CONTRIBUTING.md](CONTRIBUTING.md)). Versions
published before this change remain under GPL-3.0-or-later.
