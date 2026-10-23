# Building and running OpenAnniversary

This is the path a third party follows. It builds a native Linux renderer (SDL3 + Vulkan, no Wine/Proton at
runtime) and runs it against **your own** legally obtained copy of *Tomb Raider: Anniversary* (the 2007 cdc/Legend
GOG build). Nothing from the game ships in this repository — see `README.md` §Legal posture and `.gitignore`.

## What you need

| Dependency | Version used | Where to get it |
| --- | --- | --- |
| CMake | ≥ 3.28 (`cmake_minimum_required`) | distro package |
| A C++23 compiler | GCC 16 / Clang 23 verified; CMake picks the default | distro package |
| Ninja | 1.13.2 verified | distro package |
| zlib | 1.3.1 | `find_package(ZLIB)` |
| GoogleTest | 1.18.0 | `find_package(GTest)` (apt/gtest or distro) |
| SDL3 | CI pins `release-3.2.0`; the dev host has 3.4.18 — either works | build the pinned tag (below) or use your distro's `libSDL3` with a CMake config |
| Vulkan loader + headers | loader 1.4.x, Mesa 26.x (or `mesa-vulkan-drivers`/lavapipe for a driver-less run) | `find_package(Vulkan)` — CMake's built-in lookup finds `libvulkan-dev`; the full Vulkan SDK is **not** required |
| glslangValidator | compiles the four GLSL shaders to SPIR-V at build time (`find_program(... REQUIRED)`) | distro package (`glslang`) |
| A Vulkan-capable GPU | RX 7900 XTX (RADV) on the dev host; lavapipe software works for the smoke | driver stack (`vulkan-radeon`/`vulkan-intel`/`mesa-vulkan-drivers`) |

You also need the game install itself (read-only). Point the tools at the directory holding `bigfile.000` … `bigfile.026`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
ctest --test-dir build --output-on-failure      # unit tests: run with NO game data, so they pass anywhere
```

`oa_tests` needs no game files (fixture policy in `docs/TESTING.md` §2). A clean build is warning-clean against
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`. For the sanitizer suite (ASan+UBSan over the parser/core layer):

```sh
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOA_SANITIZE=ON -DOA_BUILD_TESTS=ON
ninja -C build-san && ctest --test-dir build-san --output-on-failure
```

If SDL3 is not already on your system, build the tag CI pins, exactly as `.github/workflows/native.yml` does:

```sh
git clone --depth 1 --branch release-3.2.0 https://github.com/libsdl-org/SDL /tmp/SDL
cmake -S /tmp/SDL -B /tmp/SDL-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/tmp/sdl3 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF
cmake --build /tmp/SDL-build && cmake --install /tmp/SDL-build
# then point the build at it:
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/tmp/sdl3   # find_package(SDL3 CONFIG)
# at runtime also export LD_LIBRARY_PATH=/tmp/sdl3/lib so the shared SDL3 loads
```

> The pinned SDL tag is the CI baseline. If your distro already ships SDL3 with a CMake config, `find_package(SDL3)`
> finds it and you can skip the source build.

## Run against your copy

All commands are read-only over the install; paths inside the archives keep the engine's backslash form (`pc-w\...`).

```sh
G="<your install dir containing bigfile.000..026>"

# Inspect the archive (this is how you find the level unit names):
./build/oa-asset "$G" list | wc -l                     # ~4304 records across 27 parts on the shipped install

# Render a real level's terrain + textures, natively (needs a Wayland/X display):
./build/oa-game --gameDir="$G" --level=cn3 --strips=true --textures=true --frames=1

# Prove the frame without any display (offscreen read-back self-check):
./build/oa-game --gameDir="$G" --mesh=8baf540e --strips=true --avatar=true --selfCheck=true --frames=1

# Keyboard-driven body over the decoded ground (W/S/A/D + Space):
./build/oa-game --gameDir="$G" --level=ma1 --strips=true --avatar=true --avatarDrive=true --selfCheck=false
```

If your machine has no presentation-capable Vulkan device, the smoke step exits 77 as a documented *skip*, not a
failure (`docs/TESTING.md` §1) — install `mesa-vulkan-drivers` (lavapipe) to make it meaningful there.

## Canonical pipeline (what CI runs)

`scripts/ci.sh` is the single source of truth — fresh configure, unit tests, renderer smoke, then the ASan/UBSan suite.
`.github/workflows/native.yml` runs the same script on an `ubuntu-24.04` runner. Set `OA_GAME_DIR` to exercise shipped
content locally; leave it unset and the smoke draws the baseline frame only:

```sh
scripts/ci.sh
OA_GAME_DIR="$G" scripts/ci.sh        # adds the shipped-geometry + traversal probes
```

## Releasing / updating

Versioned by SemVer in git tags and GitHub Releases (`CHANGELOG.md` §Releases). To move to the next revision,
`git pull` the tagged tree and rebuild — the same commands above apply. No game data is fetched at build or run time.
