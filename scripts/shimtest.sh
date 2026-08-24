#!/bin/bash
# TAA shimmer test harness: captures 8 consecutive gameplay frames with TAA
# and reports the per-pair shimmer metric (see shimmer_check.py).
#
# Why two cases
# -------------
# The scene has two independent sources of frame-to-frame change:
#   * TAA artifacts  — what we actually want to measure and drive to ~0.
#   * Legitimate motion — the trees sway in the wind (they are animated).
# If we measure shimmer with the trees swaying, the metric counts the real
# leaf motion as "shimmer" and masks the TAA artifact.  So the PRIMARY
# number is measured with the sway FROZEN (wind strength 0) and weather OFF:
# a perfectly good TAA then shows ~0 shimmer.  The ANIMATED case (default
# sway) is reported second as context: it is TAA shimmer + legitimate leaf
# motion, and is expected to be higher.
#
# Weather (snow/rain/leaves particles) is disabled in BOTH cases — falling
# particles are fast motion that would also pollute the metric.
#
# Usage:
#   ./scripts/shimtest.sh <label> [taa_weight] [taa_ghost] [wind_speed]
#
# Examples:
#   ./scripts/shimtest.sh baseline            # frozen + animated, defaults
#   ./scripts/shimtest.sh w95 0.95            # TAA blend weight 0.95
#   ./scripts/shimtest.sh gust 0.9 1.0 3.0    # faster gusts in the animated case
#
# The build must already be current (./scripts/build.sh, or ./scripts/dev_taa.sh
# to refresh just the TAA shader). Captures start 5s after load.
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
cd "$ROOT"

LABEL="${1:?label required}"
TAAW="${2:-}"
TAAG="${3:-}"
WINDP="${4:-}"          # sway angular speed for the ANIMATED case only

export ENGINE_DEBUG=1
export ENGINE_SKIP_MAIN_MENU=1
export ASAN_OPTIONS=symbolize=1:detect_leaks=1
export LSAN_OPTIONS=suppressions="$ROOT/scripts/lsan_suppress.txt"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
export ENGINE_SCREENSHOT_COUNT=8

# Run one capture+measure case.
#   run_case <casename> <weather> <wind_strength> <wind_speed>
run_case() {
    local prefix="/tmp/shim_${LABEL}_$1" weather="$2" winds="$3" windp="$4"
    rm -f "${prefix}"_*.jpg
    export ENGINE_AZGAAR_WEATHER="$weather"
    if [ -n "$winds" ]; then export ENGINE_PROPS_WIND_STRENGTH="$winds"; else unset ENGINE_PROPS_WIND_STRENGTH; fi
    if [ -n "$windp" ]; then export ENGINE_PROPS_WIND_SPEED="$windp";   else unset ENGINE_PROPS_WIND_SPEED; fi
    if [ -n "$TAAW" ];  then export ENGINE_TAA_WEIGHT="$TAAW";          else unset ENGINE_TAA_WEIGHT; fi
    if [ -n "$TAAG" ];  then export ENGINE_TAA_GHOST="$TAAG";           else unset ENGINE_TAA_GHOST; fi
    export ENGINE_SCREENSHOT="$prefix"

    timeout 60 ./build/c-game/c-game >/tmp/shim_${LABEL}_$1.log 2>&1 || true
    local n
    n=$(ls "${prefix}"_*.jpg 2>/dev/null | wc -l)
    if [ "$n" -lt 2 ]; then
        echo "  (only $n frames captured — check log)"; return 1
    fi
    python3 scripts/shimmer_check.py "$prefix" 8 --threshold 8 --out /tmp 2>&1 | sed 's/^/    /'
}

echo "== shimtest: label=$LABEL w=${TAAW:-def} g=${TAAG:-def} (weather OFF in both cases)"

echo "-- case 1/2: FROZEN sway  -> pure TAA artifact (the number to fix)"
run_case "frozen" 0 0 ""

echo "-- case 2/2: ANIMATED sway (default) -> TAA shimmer + legitimate leaf motion"
run_case "anim"   0 "" "${WINDP}"
