#!/bin/bash
# android/run_shots.sh <secs> <shot times, comma-separated> <tag>:<LARECOMP_X=1,...> ...: one scripted run per config on
# $ANDROID_SERIAL (the save list's first entry loaded), screenshots at those seconds into android/out/shots/<tag>-<t>.png,
# and the run's mean fps over its last 16 s.
set -uo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
SECS=$1 TIMES=$2; shift 2
mkdir -p android/out/shots
for cfg in "$@"; do
    tag=${cfg%%:*}; env=${cfg#*:}
    adb shell am force-stop com.botwrecomp.android
    (for t in ${TIMES//,/ }; do sleep 1; done) >/dev/null
    bash android/run_boot.sh "$SECS" "${COMMON:+$COMMON,}$env,LARECOMP_INPUT=700:A:6,820:A:6,940:A:6,1100:A:6" > /dev/null 2>&1 &
    run=$!
    start=$(date +%s)
    for t in ${TIMES//,/ }; do
        while [ $(( $(date +%s) - start )) -lt "$t" ]; do sleep 1; done
        adb exec-out screencap -p > "android/out/shots/$tag-$t.png"
    done
    wait $run
    cp android/out/boot.log "android/out/boot-$tag.log"
    fps=$(grep -o "frame [0-9]* [0-9.]* fps" "android/out/boot-$tag.log" | tail -8 | awk '{s+=$3; n++} END {if (n) printf "%.1f", s/n}')
    echo "$tag: $fps fps"
done
adb shell am force-stop com.botwrecomp.android
