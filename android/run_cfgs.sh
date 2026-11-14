#!/bin/bash
# android/run_cfgs.sh <seconds> <tag>:<LARECOMP_X=1,...> ...: one scripted run per config on $ANDROID_SERIAL (the
# save list's first entry loaded: A at 700/820/940/1100), each with the memory sampled (run_mem.sh); prints the last
# 16 s: fps, the GPU's and the render thread's ms per 2 s window (LARECOMP_VK_PROF), page-ins a second, and the
# app's PSS. Extra variables for every run: $COMMON.
set -uo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
SECS=$1; shift
for cfg in "$@"; do
    tag=${cfg%%:*}; env=${cfg#*:}
    adb shell am force-stop com.botwrecomp.android
    bash android/run_mem.sh "$tag" "$SECS" "LARECOMP_VK_PROF=1,${COMMON:+$COMMON,}$env,LARECOMP_INPUT=700:A:6,820:A:6,940:A:6,1100:A:6" > /dev/null
    log=android/out/boot-$tag.log
    fps=$(grep -o "frame [0-9]* [0-9.]* fps" "$log" | tail -8 | awk '{s+=$3; n++} END {if (n) printf "%.1f", s/n}')
    gpu=$(grep -o "| gpu [0-9]* cpu exec [0-9]*" "$log" | tail -8 | awk '{g+=$3; c+=$6; n++} END {if (n) printf "gpu %.0f exec %.0f", g/n, c/n}')
    flt=$(grep -E "^t=" android/out/mem-$tag.txt | grep -o "majflt [0-9]*" | tail -3 | awk '{v[NR]=$2} END {if (NR>1) printf "%.0f", (v[NR]-v[1])/((NR-1)*15)}')
    pss=$(grep -E "^t=" android/out/mem-$tag.txt | grep -o "TOTAL PSS: [0-9]*" | tail -1 | awk '{printf "%.0f", $3/1024}')
    alive=$(adb shell pidof com.botwrecomp.android | tr -d '\r')
    echo "$tag: $fps fps, $gpu ms/2s, $flt page-ins/s, PSS ${pss} MB, ${alive:+alive}${alive:-DEAD}"
done
adb shell am force-stop com.botwrecomp.android
