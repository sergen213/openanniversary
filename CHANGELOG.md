# Changelog

All notable changes to this project are documented here. The format follows Keep a Changelog; the project starts at
zero version and has no releases yet.

## [Unreleased]

### Added

- CMake project (C++23, GCC 16 / Clang 23 verified) with an `oa_cdc` static library, the `oa-asset` tool and a
  GoogleTest target that runs without any game data.
- cdc `bigfile` container reader: index parsing with an unsorted-table refusal, ASCII-case-folded CRC-32 path
  hashing (poly `0x04C11DB7`, init/final `0xFFFFFFFF`, non-reflected), sector/part addressing with the
  Legend/Anniversary alignment `0x9600000`, specialisation-mask record selection, zlib and raw-deflate inflation for
  compressed records, and reads that straddle a part boundary.
- `oa-asset` subcommands `list`, `hash`, `resolve <paths.txt>` and `extract --path|--hash [--mask]`.
- Documentation: `README.md`, `docs/ENVIRONMENT.md` (verified host/toolchain facts), `docs/formats/CONTAINERS.md`
  (format with function addresses and evidence tags), `docs/ROADMAP.md` (milestone status).

### Verified

- Unit tests: 7 passing, including golden path hashes taken from the shipped archive
  (`pc-w\generalbank.drm` → `0xB3AAAFD6`, `pc-w\lara.drm` → `0x5FF91900`).
- Real install (`tra.exe` sha256 `b43ade0d…ea53a`, GOG build): 4,304 records across 27 parts; hash table sorted; no
  record straddles a part boundary; every PC record stored uncompressed.
- Name recovery: 948 of the 949 object-list units resolve as `pc-w\<name>.drm`.
- Extraction round trip: `pc-w\generalbank.drm` (90,384 bytes) and `pc-w\lara.drm` (7,630,042 bytes), both starting
  with the DRM version field value 14 (Legend/Anniversary unit format).

### Not yet implemented

- Window creation, logging, configuration, Vulkan initialisation and any rendered frame (Milestone 2).
- Any payload-level parser beyond the container: DRM v14 sections, textures, sound, cinematics.
