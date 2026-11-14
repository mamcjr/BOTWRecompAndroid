#!/bin/bash
# android/run_ver.sh <v160|v182> [seconds] [LARECOMP_X=1,...]: android/run_boot.sh for another version of the game, kept
# beside v1.0.0 - its files in <external files>/<version>/extracted (android/push_game.sh with GAME_DEST and
# GAMELIB_OUT=android/out/<version>), its game code copied into the app's internal storage, since Android loads no code
# from external storage, and both handed to the runtime (LARECOMP_DATA_ROOT, LARECOMP_GAME_LIB). Saves and settings live
# in that data root too; with none there yet, the runtime starts the save as a copy of v1.0.0's (LARECOMP_SAVE_SEED).
set -euo pipefail
export MSYS_NO_PATHCONV=1  # (Git Bash: leave the device paths alone)
cd "$(dirname "$0")/.."
VER=${1:?usage: android/run_ver.sh <v160|v182> [seconds] [LARECOMP_X=1,...]}
PKG=com.zbotw.recomp
EXT=/sdcard/Android/data/$PKG/files/$VER
INT=/data/data/$PKG/files/$VER
adb shell am force-stop $PKG
adb shell run-as $PKG mkdir -p files/$VER
# (run-as may not read external storage, nor take a redirect: the shell reads it and pipes it in)
for f in botwgame.so botwgame.so.id; do adb shell "cat $EXT/extracted/$f | run-as $PKG sh -c 'cat > files/$VER/$f'"; done
# (the group shared by the app and adb, inherited: what the app writes there - captures, debug frames - adb can read)
adb shell "mkdir -p $EXT/build/frames && chgrp -R ext_data_rw $EXT/build && chmod -R 2777 $EXT/build" 2>/dev/null || true
bash android/run_boot.sh "${2:-120}" \
    "LARECOMP_DATA_ROOT=$EXT,LARECOMP_GAME_LIB=$INT/botwgame.so,LARECOMP_SAVE_SEED=/sdcard/Android/data/$PKG/files/save${3:+,$3}"
