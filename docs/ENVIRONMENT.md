# Development environment (verified facts)

Recorded so build failures can be separated from host assumptions. Re-measure with the listed commands before
changing anything toolchain-related.

## Host

| Item | Value | Source |
| --- | --- | --- |
| Distribution | CachyOS (Arch-based, rolling), kernel 7.2.11-1-cachyos | `cat /etc/os-release`, `uname -a` |
| CPU | AMD Ryzen 9 9950X3D, 16 cores / 32 threads | `lscpu` |
| RAM | ~30 GiB total; keep Ghidra headless runs serialised (one instance at a time) | `free -g` |
| GPU | AMD Navi 31 [RX 7900 XT/XTX] `1002:744c`, driver `amdgpu`, Mesa/RADV | `lspci -nn`, `lsmod` |
| Vulkan | loader 1.4.363, Mesa 26.2.4 (`vulkan-radeon`) | `vulkaninfo --summary`, `pacman -Qs vulkan` |
| Repositories | game install + this repo on `/mnt/2nd4TB` (ext4, 3.7 T) | `df -h` |

Note: a stale `VK_LAYER_…lsfg-vk-layer.so` entry makes the loader print
`Failed to find 'vkGetInstanceProcAddr' in layer "liblsfg-vk-layer.so"` on every Vulkan program start. The layer is
skipped, so it is noise rather than a fault; remove the leftover layer manifest if validation runs get confusing.

## Toolchain (as installed at time of writing)

| Tool | Version | Check |
| --- | --- | --- |
| GCC | 16.2.1 20260810 | `gcc --version` |
| Clang | 23.1.3 | `clang --version` |
| CMake | 4.4.4 | `cmake --version` |
| Ninja | 1.13.2 | `ninja --version` |
| Python | 3.14.8 | `python3 --version` |
| Ghidra | packaged (`/usr/bin/ghidra`) | `ghidra --version` |
| zlib | 1.3.1 | CMake `find_package(ZLIB)` output |
| GoogleTest | 1.18.0 | CMake `find_package(GTest)` output |
| SDL3 | 3.4.18 (`sdl2-compat` 2.32.74 also present) | `pkg-config --modversion sdl3` |
| Vulkan headers / loader / tools / validation | 1.4.363.x | `pacman -Q vulkan-headers volk glslang` |
| glslangValidator / glslc / spirv-val | present (`1:1.4.363.0`) | `command -v` |
| lld / ld.lld | present; `mold`, `dxcompiler`, `dxc` absent | `command -v` |

C++ standard in use: `-std=c++23` (compilers also accept newer modes, but C++23 is the floor we build against).
Warning set: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`. Debug builds get ASan/UBSan with `-DOA_SANITIZE=ON`.

## Game install used for verification

`/mnt/2nd4TB/TombRaiderAnniversary/Tomb Raider Anniversary`, 3.9 GB, GOG build:
`tra.exe` sha256 `b43ade0d922b0f007747e5707b97c86404cbf4f051829e3711f7007a656ea53a`,
`bigfile.000` … `bigfile.026`, plus `d3d9.dll`, `dxwrapper.dll`, `binkw32.dll`, `DEPHook.asi` (third-party wrappers,
not part of the 2007 release and not used by this project).

Read-only rule: tools open these files with `"rb"` only. Extracted payloads go to a scratch directory outside both
the install and this repository (`/tmp`).
