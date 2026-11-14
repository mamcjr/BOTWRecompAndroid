#!/bin/bash
# android/push_game.sh <game folder with exefs/ and romfs/>: the player's own game files onto the device over adb,
# set up as the start screen's setup would (docs/las/ANDROID-PORT.md, Fase 2; until the app has a folder picker):
# <data root>/extracted/{exefs,romfs} (skipped when there), romfs.bin built on the device (android/tools/romfs_tool.c,
# no third copy of 6 GB through USB), and the game code from android/build_gamelib.sh (android/out).
set -euo pipefail
export MSYS_NO_PATHCONV=1  # (Git Bash: leave the device paths alone)
cd "$(dirname "$0")/.."
R=$PWD
SRC=${1:?usage: android/push_game.sh <folder with exefs/ and romfs/>}
# (GAME_DEST / GAMELIB_OUT: another game version in its own data root, e.g. 1.6.0 in files/v160 - android/run_v160.sh)
D=${GAME_DEST:-/sdcard/Android/data/com.zbotw.recomp/files/extracted}
LIB=${GAMELIB_OUT:-android/out}
DEV_TMP=/data/local/tmp/botwrecompandroid

SDK=${ANDROID_HOME:-${LOCALAPPDATA:-$HOME}/Android/Sdk}
SDK=${SDK//\\//}
NDK=${ANDROID_NDK:-$SDK/ndk/30.0.16248370}
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64; EXE=.exe ;; Darwin) HOST=darwin-x86_64; EXE= ;; *) HOST=linux-x86_64; EXE= ;; esac
CC=$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang$EXE

adb shell mkdir -p "$D" "$DEV_TMP"
for part in exefs romfs; do
    if [ -z "$(adb shell ls "$D/$part" 2>/dev/null | head -1)" ]; then
        echo "copying $part..."
        adb push "$SRC/$part" "$D/"
    fi
done

if ! adb shell test -s "$D/romfs.bin"; then
    mkdir -p android/out
    "$CC" --target=aarch64-linux-android30 -O2 -Iruntime -o android/out/romfs_tool android/tools/romfs_tool.c runtime/romfs_build.c
    adb push android/out/romfs_tool "$DEV_TMP/" >/dev/null
    adb shell chmod 755 "$DEV_TMP/romfs_tool"
    echo "building romfs.bin on the device..."
    adb shell "$DEV_TMP/romfs_tool" "$D/romfs" "$D/romfs.bin"
fi

if [ -f "$LIB/botwgame.so" ]; then
    adb push "$LIB/botwgame.so" "$LIB/botwgame.so.id" "$D/"
else
    echo "no $LIB/botwgame.so yet (android/build_gamelib.sh)"
fi
# what adb pushes belongs to the shell user (rwxrws---): readable by the app only once others may read it (the app's
# own setup makes these files as the app)
adb shell chmod -R o+rX "$D"
adb shell ls -la "$D"
