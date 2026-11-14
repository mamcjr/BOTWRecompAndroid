#!/bin/bash
# android/run_burst.sh <tag> <start s> <shots> <LARECOMP_X=1,...>: a scripted run on $ANDROID_SERIAL (the save list's
# first entry loaded, then the extra input in the variables), a burst of screenshots from <start> s into
# android/out/burst/<tag>-NN.png (as fast as screencap goes, ~1 a second).
set -uo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
TAG=$1 START=$2 N=$3 ENVS=${4:-}
mkdir -p android/out/burst
adb shell am force-stop com.botwrecomp.android
bash android/run_boot.sh $((START + N + 10)) "$ENVS" > /dev/null 2>&1 &
run=$!
t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt "$START" ]; do sleep 1; done
for i in $(seq -w 1 "$N"); do adb exec-out screencap -p > "android/out/burst/$TAG-$i.png"; done
wait $run
cp android/out/boot.log "android/out/boot-$TAG.log"
adb shell am force-stop com.botwrecomp.android
