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
# With a real install available, draw shipped geometry too: one level unit's unindexed vertex run plus its terrain strip
# records reached through the oct-tree leaves (`--strips=true`, the level unit hash is documented in
# docs/formats/GEOMETRY.md §6 and §15; OA_MESH_RECORD overrides it for another unit). `--avatar=true` runs the movement core
# (gravity, ground contact with step-up, one jump) over that same gated geometry, so neither the query nor its harness can rot
# silently - GEOMETRY.md §16-§19 says why those heights are reported rather than asserted. `--avatarTraversal=true` then asks
# whether one take-off clears a hole that unit actually leaves (docs/TESTING.md §4).
[[ -n "${OA_GAME_DIR:-}" ]] && game_args+=(--gameDir="$OA_GAME_DIR" --mesh="${OA_MESH_RECORD:-8baf540e}" --strips=true --avatar=true --avatarTraversal=true)
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
# Two signatures mean the display has no presentation-capable Vulkan device. SDL3's own pre-flight check on an
# Xvfb without DRI3 says "SDL_Vulkan_CreateSurface: The specified window isn't a Vulkan window"; our renderer says
# "no graphics+present queue" when no swapchain-capable device answers. Both are environment gaps (GH's ubuntu-24.04
# xvfb setup is one of them), not regressions, and both make the smoke a documented skip; the ASan/UBSan unit suite
# below still runs the parser/core layer under sanitizers on the same host. Install lavapipe or run on a real Wayland
# session to make this step meaningful on the machine that lacks the presentation stack.
if [[ $status -ne 0 ]] && grep -qE 'SDL_Vulkan_CreateSurface|no graphics\+present queue' "$log"; then
    echo "SKIP: no presentation-capable Vulkan device on this display (exit 77)" >&2
    sed -n '1,15p' "$log" >&2
    exit 77
fi
if [[ $status -ne 0 ]]; then
    sed -n '1,40p' "$log" >&2
    exit "$status"
fi
cat "$log"
if [[ -n "${OA_GAME_DIR:-}" ]]; then
    # The traversal probe has to say something definite about the unit it scanned. Silence is how its own bad probes hid at
    # first: a candidate set built from ceilings read as floors, and a body that detached on the first downhill step.
    if grep -q 'crossed the hole and stands on the far side' "$log"; then
        echo "traversal: verified crossing over $(grep -c 'attempt .* in' "$log" | tr -d ' ') candidate(s) scanned"
    elif grep -q 'found no hole of half a cell' "$log"; then
        echo "traversal: this unit's gated coverage holds nothing jumpable (stated, not guessed)"
    else
        echo "FAIL: traversal reported neither a crossing nor an empty candidate set" >&2
        exit 1
    fi
fi
echo "== 3/3 unit tests under ASan+UBSan =="
# Display path is excluded here on purpose: vendor code (SDL/driver) leaks by design and would mask real findings.
configure "$scratch/san" ON
cmake --build "$scratch/san"
ctest --test-dir "$scratch/san" --output-on-failure

echo "ci.sh: all steps passed"
