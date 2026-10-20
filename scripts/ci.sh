#!/usr/bin/env bash
# Canonical build/test pipeline. Keep .github/workflows/native.yml in sync with this script.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/oa-ci-XXXXXX")
trap 'rm -rf "$scratch"' EXIT

configure() { # configure <build-dir> <sanitizer: ON|OFF>
    cmake -S "$root" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DOA_BUILD_TESTS=ON -DOA_SANITIZE="$2"
}

echo "== 1/3 unit tests (no sanitizers) =="
configure "$scratch/plain" OFF
cmake --build "$scratch/plain"
ctest --test-dir "$scratch/plain" --output-on-failure

echo "== 2/3 renderer smoke =="
game_args=()
[[ -n "${OA_GAME_DIR:-}" ]] && game_args+=(--gameDir="$OA_GAME_DIR")
log="$scratch/smoke.log"; status=0
if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
    "$scratch/plain/oa-game" --selfCheck=true --frames=3 "${game_args[@]}" >"$log" 2>&1 || status=$?
elif command -v xvfb-run >/dev/null 2>&1; then
    SDL_VIDEODRIVER=x11 xvfb-run -a -s "-screen 0 1280x800x24" \
        "$scratch/plain/oa-game" --selfCheck=true --frames=3 "${game_args[@]}" >"$log" 2>&1 || status=$?
else
    echo "SKIP: no display server available for the renderer smoke (exit 77)" >&2
    exit 77
fi
if [[ $status -ne 0 ]] && grep -q "no graphics+present queue" "$log"; then
    # Xvfb without DRI3, or a runner with no Vulkan ICD at all, cannot present. That is an environment gap, not a
    # regression; install lavapipe (Mesa software Vulkan) to make this step meaningful there.
    echo "SKIP: no presentation-capable Vulkan device on this display (exit 77)" >&2
    sed -n '1,15p' "$log" >&2
    exit 77
fi
if [[ $status -ne 0 ]]; then
    sed -n '1,40p' "$log" >&2
    exit "$status"
fi
cat "$log"
echo "== 3/3 unit tests under ASan+UBSan =="
# Display path is excluded here on purpose: vendor code (SDL/driver) leaks by design and would mask real findings.
configure "$scratch/san" ON
cmake --build "$scratch/san"
ctest --test-dir "$scratch/san" --output-on-failure

echo "ci.sh: all steps passed"
