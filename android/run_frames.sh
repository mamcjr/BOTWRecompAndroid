#!/bin/bash
# android/run_frames.sh <tag> <from frame> <secs> <LARECOMP_X=1,...>: a scripted run on $ANDROID_SERIAL saving every
# presented frame from <from> (LARECOMP_SCREENSHOT_EVERY=1, slow: ~1 a second) into a fresh folder, pulled to
# android/out/frames/<tag>/ (app-owned folders can't be emptied by adb: each run writes under build/f-<tag>).
set -uo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
TAG=$1 FROM=$2 SECS=$3 ENVS=${4:-}
D=/sdcard/Android/data/com.botwrecomp.android/files/v190/build
adb shell am force-stop com.botwrecomp.android
bash android/run_boot.sh "$SECS" "LARECOMP_SCREENSHOT_EVERY=1,LARECOMP_SCREENSHOT_FROM=$FROM,$ENVS" > /dev/null 2>&1
adb shell am force-stop com.botwrecomp.android
rm -rf "android/out/frames/$TAG"
mkdir -p "android/out/frames/$TAG"
adb pull -a "$D/frames/." "android/out/frames/$TAG/" > /dev/null
find "android/out/frames/$TAG" -name "*.png" ! -newer android/out/boot.log -mmin +6 -delete 2>/dev/null
cp android/out/boot.log "android/out/boot-f-$TAG.log"
ls "android/out/frames/$TAG" | wc -l
