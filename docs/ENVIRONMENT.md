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
| Wine (research tool, never a runtime) | `wine-11.20` | `wine --version` |
| DXVK 3.1 (inside the Wine prefix only) | `~/.config/heroic/tools/dxvk/dxvk-3.1` | directory listing |
| RenderDoc (research tool; Linux build injects nothing into Wine guests, see below) | 1.46 | `renderdoccmd --version` |

C++ standard in use: `-std=c++23` (compilers also accept newer modes, but C++23 is the floor we build against).
Warning set: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`. Debug builds get ASan/UBSan with `-DOA_SANITIZE=ON`.

## Game install used for verification

`/mnt/2nd4TB/TombRaiderAnniversary/Tomb Raider Anniversary`, 3.9 GB, GOG build:
`tra.exe` sha256 `b43ade0d922b0f007747e5707b97c86404cbf4f051829e3711f7007a656ea53a`,
`bigfile.000` … `bigfile.026`, plus `d3d9.dll`, `dxwrapper.dll`, `binkw32.dll`, `DEPHook.asi` (third-party wrappers,
not part of the 2007 release and not used by this project).

Read-only rule: tools open these files with `"rb"` only. Extracted payloads go to a scratch directory outside both
the install and this repository (`/tmp`).

### Running the original for observation (private, never committed)

The exe is 32-bit PE, so it runs only under Wine. Reproduce on this host [V-data]:

```sh
# a copy keeps the purchased install pristine; the prefix and copy live in the private work dir
cp -a "/mnt/2nd4TB/TombRaiderAnniversary/Tomb Raider Anniversary" ../openanniversary-work/wine-drive/trawork
WINEPREFIX=$PWD/../openanniversary-work/wine-tra wineboot -u                       # 441 MB prefix
D=~/.config/heroic/tools/dxvk/dxvk-3.1                                             # DXVK already on the machine
cp $D/x32/*.dll ../openanniversary-work/wine-tra/drive_c/windows/syswow64/          # d3d8, d3d9, dxgi (32-bit)
cp $D/x64/*.dll ../openanniversary-work/wine-tra/drive_c/windows/system32/
cd ../openanniversary-work/wine-drive/trawork && DISPLAY=:0 wine ./tra.exe          # window titled
                                                                                  # "Tomb Raider: Anniversary" appears
```

Measured [V-data]: the game boots to its menu, and one `xdotool key Down` plus `Return` (window activated first) starts a
new game - a desktop screenshot (`spectacle -b -n`, see below) shows Lara standing in Croft Manor's entrance hall, so real
level assets are loaded and on screen. RSS: 214 MB at the menu, ~259-265 MB in-level. The install's own `d3d9.dll` and
`dxwrapper.dll` shims stay in front of DXVK as shipped; the process survives indefinitely.

Two runtime-evidence routes were tried and both need more work than they look:

- **GPU capture:** RenderDoc 1.45 (Arch package; now 1.46) produced **no file at all** when launched as
  `renderdoccmd capture -w … wine ./tra.exe` - the Linux build does not inject into Wine guest processes. Capturing D3D
  traffic needs a Windows RenderDoc inside the prefix, or DXVK-side hooks.
- **Address-space reading:** `/proc/<pid>/mem` returns `EACCES`. Kernel Yama is at `ptrace_scope = 1`, and Wine's PE
  processes are children of `wineserver`; even a wineserver we start ourselves leaves the guest reparented to
  `systemd --user`, so no process we start ends up an ancestor of it. Wine's debugger cannot stand in either: this Wine
  (11.20) has **no `memsave` command**, that string appears in none of its binaries, and the earlier note pointing at it
  was never verified [V-data].

What works instead is standing inside the guest with a d3d9 shim - `tools/dxhook/`:

```sh
cd tools/dxhook && ./build.sh                      # needs i686-w64-mingw32-gcc (pacman -S mingw-w64-gcc)
WINEPREFIX=$PWD/../../openanniversary-work/wine-tra ./install.sh install   # DXVK moves to d3d9_dxvk.dll
cd ../../openanniversary-work/wine-drive/trawork
WINEDLLOVERRIDES="d3d9=n,b" DISPLAY=:0 WINEPREFIX=.../wine-tra wine ./tra.exe   # override REQUIRED: Wine treats
                                                                                # d3d9 as a builtin, and without it the
                                                                                # shim is never loaded (no log appears)
python3 ../../../../openanniversary/tools/dxhook/report.py .../wine-tra/drive_c/tra/d3d9log
./install.sh uninstall                             # put DXVK back afterwards
```

Measured [V-data]: `/proc/<pid>/maps` shows the chain `trawork/d3d9.dll` (the install's own vendor shim, left untouched)
`wine-tra/drive_c/tra/d3d9log`, and one four-minute session at menu plus entrance hall produced 17.9 M draw lines and
1.1 GB of text - stop the game once the level is loaded. Buffer contents only appear as `vb_*.bin`/`ib_*.bin` when a
resource can actually be locked, which default-pool terrain buffers cannot: that capture logged zero dump files.

Milestone-5 research added a slot probe to the shim (`OA_PROBE_SLOT=<n>` patches only vtbl slot n with a
hook that logs its sixteen float arguments and forwards). Measured negative [V-data] (2026-10-22): nine
runs of ~30-80 s reaching the hall — with `OA_PROBE_SLOT` at the header's `SetViewport`-neighbour slots
201±4, at the past-header-declared slots 288..292, and even at slot 82, the one confirmed as `DrawIndexedPrimitive`
by the October capture — logged **zero** draw or probe lines, while that October session logged 17.9 M of them.
Whatever differs between that capture's session and these runs, the d3d9 device path stops being reached; it is
isolated first before any motion measurement is attempted. The prefix chain, the run line and the install script
are unchanged from the working recipe above.

Measured (2026-10-22, second attempt at the motion session, `openanniversary-work/run-walk-measure2.sh`): the launcher
popup now titled **`Tomb Raider: Anniversary - Setup`** (426x367 client area at 1707,910; "Rendering Setup" is only its
drawn caption, so the October scripts' `xdotool search --name 'Rendering Setup'` finds nothing) does open the game —
synthetic clicks at its drawn Ok button reach nothing while a keyboard-Escape march does: the game window
`Tomb Raider: Anniversary` appeared and Down+Return on the activated window still starts a new game as in October. What
the screenshots show is not usable for motion, though: every desktop frame is Wine's full-screen `wine-desktop` ring
pattern with only terminal windows on top — no menu, no hall — so the guest renders into something the XWayland desktop surface
no longer composites where the screenshots can see it. But the capture instrument is fixed by one falsifier [V-data]
(2026-10-22, `openanniversary-work/wincap-test.sh`): `import -window <id>` aimed at the Wine window ID grabs its pixels
exactly - the same window that is only rings in a desktop screenshot comes out complete. Window-target capture, not
desktop capture, is what motion measurement must use. The same session then settled why the game never opens headless [V-data]
(2026-10-22, three sessions): the launcher dialog answers nothing synthetic — a 48-click grid over
its Ok button area (with `windowraise`/`windowfocus` first), `xdotool key --window <id>` of Return/Enter/Tab/space, and the
Escape march all leave the popup open, and no window titled exactly `Tomb Raider: Anniversary` ever appears; a real mouse
click on Ok (done manually by the user, 2026-10-22) does open it. `ydotool` (Wayland-level uinput clicks) is not installed
on this host. So the shipped-game motion cross-check needs the user's Ok click in the session, or ydotool installed,
before any window-grab can measure walking. The desktop frames showing Wine's ring wallpaper over everything are the
virtual-desktop state the earlier October sessions did not have — treat as environment drift to re-isolate if manual
launch also shows rings. Motion stays unmeasured.

One durable find came out of the same prefix while diagnosing that popup: `wine-tra/user.reg` holds the shipped game's
own settings tree (`HKCU\Software\Crystal Dynamics\Tomb Raider: Anniversary` plus `\Graphics` and `\Input` subsections),
written by an earlier session — the option names and value types the shipped renderer actually carries (`EnableShadows`,
`EnableReflection`, `EnableDepthOfField`, `EnableWaterFX`, `EnableVSync`, `AntiAliasing`, `Contrast`, `Brightness`,
`UseShader30`, `CreateGameFourCC`, volumes as dwords, aim/camera-invert flags, a `Gamepad` string table). That is the
feature-flag vocabulary for Milestone 7 and a FourCC hint for the container research; recorded in
`docs/RESEARCH.md` §7. [V-data] — reading our own prefix; the launch-semantics half of it (settings live in that
registry key, `tra.exe -setup` opens the dialog) is prior-community [S] via pcgamingwiki.com's TR Anniversary page.

## Display and visual verification

* KDE Plasma on Wayland (`$WAYLAND_DISPLAY=wayland-1`, compositor `kwin_wayland`); an XWayland bridge answers at
  `DISPLAY=:0`. SDL3 opens native Wayland windows, so the window/renderer work was verified with no X11 fallback. [V-data]
* Working screenshot tool: `spectacle -b -n -o out.png` (KDE, via the compositor's D-Bus interface). ImageMagick
  `import -window root` fails here with "missing an image filename", and this ffmpeg build has no `x11grab` input, so
  neither is usable for verification. [V-data]
* Grab at full desktop resolution (3840x2160 here, scale factor 1) and *measure pixels* instead of eyeballing. A plain
  colour bounding box is misleading because other UI elements share the hue; find the largest solid rectangle of runs
  instead — for a 1600x900 window that measured exactly 1600x893 px, the missing rows being under the panel. [V-data]
* An sRGB attachment encodes clear values: linear `(0.10, 0.35, 0.62)` arrives on screen as sRGB `#59A0CF`. [V-data]
