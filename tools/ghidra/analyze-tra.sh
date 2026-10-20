#!/usr/bin/env bash
# Headless Ghidra import + auto-analysis + exports for tra.exe (Tomb Raider: Anniversary PC).
#
# Usage: tools/ghidra/analyze-tra.sh [--overwrite] [--decompile-all] [--exports-only]
#
# Private work dir ($OA_WORK/ghidra) holds the project, logs and decompilation output.
# Those artifacts are derived from a proprietary executable: never commit them, never
# redistribute them. The game directory is only ever read.
#
# Env: GHIDRA_HOME (default /opt/ghidra), TRA_EXE, OA_WORK, GHIDRA_HEADLESS_MAXMEM (default 3G).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GHIDRA_HOME="${GHIDRA_HOME:-/opt/ghidra}"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
TRA_EXE="${TRA_EXE:-/mnt/2nd4TB/TombRaiderAnniversary/Tomb Raider Anniversary/tra.exe}"
OA_WORK="${OA_WORK:-$(cd "$REPO_ROOT/.." && pwd)/openanniversary-work}"

GHIDRA_WORK="$OA_WORK/ghidra"
PROJECT_DIR="$GHIDRA_WORK/project"
PROJECT_NAME="tra"
PROGRAM_NAME="$(basename "$TRA_EXE")"
EXPORT_DIR="$GHIDRA_WORK/exports"
LOG_DIR="$GHIDRA_WORK/logs"
ANALYZED_STAMP="$GHIDRA_WORK/.analyzed"

# 3G keeps several JVMs + sanitizer builds inside a 30 GiB host. Raise only if analysis runs out of heap.
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-3G}"

[[ -x "$HEADLESS" ]] || { echo "analyzeHeadless not found at $HEADLESS (set GHIDRA_HOME)" >&2; exit 1; }
[[ -r "$TRA_EXE" ]] || { echo "cannot read $TRA_EXE" >&2; exit 1; }
case "$OA_WORK" in "$(dirname "$TRA_EXE")"*) echo "refusing: OA_WORK is inside the game directory" >&2; exit 1 ;; esac

OVERWRITE=0; DECOMPILE_ALL=0; EXPORTS_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --overwrite) OVERWRITE=1 ;;
        --decompile-all) DECOMPILE_ALL=1 ;;
        --exports-only) EXPORTS_ONLY=1 ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

mkdir -p "$PROJECT_DIR" "$EXPORT_DIR" "$LOG_DIR"
sha256sum "$TRA_EXE" | cut -d' ' -f1 > "$GHIDRA_WORK/input.sha256"

POST=(-postScript ExportStrings.java "$EXPORT_DIR"
      -postScript ExportFunctions.java "$EXPORT_DIR"
      -postScript ExportImports.java "$EXPORT_DIR"
      -postScript ExportRtti.java "$EXPORT_DIR" apply)
[[ $DECOMPILE_ALL -eq 1 ]] && POST+=(-postScript DecompileAll.java "$EXPORT_DIR/decomp_all.c")

COMMON=("$PROJECT_DIR" "$PROJECT_NAME" -scriptPath "$SCRIPT_DIR/scripts"
        -log "$LOG_DIR/headless.log" -scriptlog "$LOG_DIR/scripts.log")

program_exists() { [[ -f "$ANALYZED_STAMP" && -f "$PROJECT_DIR/$PROJECT_NAME.gpr" ]]; }

headless() {  # retry while another process holds the project lock (~5 min)
    local out rc=0 try
    out="$(mktemp)"
    for try in $(seq 1 60); do
        rc=0; "$HEADLESS" "$@" >"$out" 2>&1 || rc=$?
        grep -q "Unable to lock project" "$out" || break
        echo "[ghidra] project locked; retry $try/60" >&2; sleep 5
    done
    cat "$out"; rm -f "$out"; return $rc
}

if [[ $OVERWRITE -eq 0 ]] && program_exists; then EXPORTS_ONLY=1; fi

if [[ $EXPORTS_ONLY -eq 1 ]]; then
    program_exists || { echo "no analyzed program in $PROJECT_DIR; run without --exports-only" >&2; exit 1; }
    echo "[analyze] re-running exports only"
    headless "${COMMON[@]}" -process "$PROGRAM_NAME" -noanalysis "${POST[@]}"
else
    echo "[analyze] importing + analyzing $TRA_EXE (heap $GHIDRA_HEADLESS_MAXMEM)"
    headless "${COMMON[@]}" -import "$TRA_EXE" -overwrite \
        -processor "x86:LE:32:default" -cspec windows \
        -analysisTimeoutPerFile 7200 "${POST[@]}"
    touch "$ANALYZED_STAMP"
fi

echo "[analyze] exports:"; ls -la "$EXPORT_DIR"
