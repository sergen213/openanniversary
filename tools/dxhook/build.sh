#!/bin/sh
# Build the 32-bit d3d9 logging shim for observing the shipped engine (see shim.c).
#
# The generated .def re-exports every name DXVK's d3d9.dll exports, at DXVK's own ordinals, as loader
# forwarders to d3d9_dxvk.dll - except Direct3DCreate9/Ex, which this shim implements so it can reach
# the device object. Which names matter is measured, not guessed: binkw32.dll in the install imports
# exactly one (Direct3DCreate9), and everything else is forwarded for safety.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DXVK=${DXVK:-$HOME/.config/heroic/tools/dxvk/dxvk-3.1/x32/d3d9.dll}
out=$here/build
mkdir -p "$out"

[ -f "$DXVK" ] || { echo "no DXVK d3d9 at $DXVK (set DXVK=...)" >&2; exit 1; }

# objdump prints "[ idx] +base[ORD] hint NAME"; ORD is DXVK's own ordinal. No BASE directive: GNU ld
# rejects it, so our export base becomes 1. Measured consequence-free - the only static importer of
# d3d9 in the install (binkw32.dll) names Direct3DCreate9 instead of using an ordinal.
names=$(i686-w64-mingw32-objdump -p "$DXVK" | sed -n 's/^\t\[ *[0-9]*\] +base\[ *\([0-9]*\)\] *[0-9a-f]* \([A-Za-z_][A-Za-z0-9_@]*\)$/\2 \1/p')

{
    echo 'LIBRARY d3d9.dll'
    echo 'EXPORTS'
    printf '    Direct3DCreate9 = hook_Direct3DCreate9@4 @%s\n' "$(printf '%s\n' "$names" | awk '$1=="Direct3DCreate9"{print $2}')"
    printf '    Direct3DCreate9Ex = hook_Direct3DCreate9Ex@8 @%s\n' "$(printf '%s\n' "$names" | awk '$1=="Direct3DCreate9Ex"{print $2}')"
    printf '%s\n' "$names" | while read -r n o; do
        case $n in Direct3DCreate9|Direct3DCreate9Ex) continue ;; esac
        printf '    %s = d3d9_dxvk.%s @%s\n' "$n" "$n" "$o"
    done
} >"$out/d3d9.def"

i686-w64-mingw32-gcc -O2 -Wall -Wextra -fno-strict-aliasing -shared -static-libgcc \
    -o "$out/d3d9.dll" "$here/shim.c" "$out/d3d9.def" -lkernel32

echo "built $out/d3d9.dll with $(sed -n 's/^    \([A-Za-z_][A-Za-z0-9_@]*\) =.*/\1/p' "$out/d3d9.def" | wc -l) exports"
i686-w64-mingw32-objdump -p "$out/d3d9.dll" | grep -E '^\s+\[\s*[0-9]+\].*(Direct3DCreate9|d3d9_dxvk)' | head -5
