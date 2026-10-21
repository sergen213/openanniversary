#!/bin/sh
# Put the logging shim in front of DXVK inside the private prefix, keeping every vendor shim the
# install shipped. Chain after this script: tra.exe -> trawork/d3d9.dll (vendor, untouched) ->
# syswow64/d3d9.dll (ours) -> syswow64/d3d9_dxvk.dll (DXVK). Uninstall restores the original names.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${WINEPREFIX:-$here/../../../openanniversary-work/wine-tra}
sys=$prefix/drive_c/windows/syswow64
bak=$here/../../../openanniversary-work/dxshim-backup

case ${1:-install} in
install)
    [ -f "$here/build/d3d9.dll" ] || { echo "run build.sh first" >&2; exit 1; }
    mkdir -p "$bak"
    if [ -f "$sys/d3d9.dll" ] && [ ! -f "$bak/d3d9.dll.orig" ]; then
        cp -a "$sys/d3d9.dll" "$bak/d3d9.dll.orig"
        echo "backed up DXVK d3d9.dll to $bak"
    fi
    if [ ! -f "$bak/d3d9.dll.orig" ] && [ ! -f "$sys/d3d9_dxvk.dll" ]; then
        echo "prefix has no DXVK d3d9.dll to take over; install it first (docs/ENVIRONMENT.md)" >&2
        exit 1
    fi
    cp -a "$bak/d3d9.dll.orig" "$sys/d3d9_dxvk.dll"
    rm -f "$sys/d3d9.dll"
    cp "$here/build/d3d9.dll" "$sys/d3d9.dll"
    echo "installed: $sys/d3d9.dll is the shim, DXVK answers as d3d9_dxvk.dll"
    ;;
uninstall)
    [ -f "$bak/d3d9.dll.orig" ] || { echo "no backup at $bak" >&2; exit 1; }
    rm -f "$sys/d3d9.dll" "$sys/d3d9_dxvk.dll"
    cp -a "$bak/d3d9.dll.orig" "$sys/d3d9.dll"
    echo "restored DXVK as $sys/d3d9.dll"
    ;;
*)
    echo "usage: install.sh [install|uninstall]" >&2
    exit 2
    ;;
esac
