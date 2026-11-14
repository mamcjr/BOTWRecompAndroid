#!/bin/bash
# android/run_boot.sh [seconds] [LARECOMP_X=1,...]: starts the game on the device straight past the start screen
# (LARECOMP_NO_LAUNCHER=1, plus any extra runtime switches), records logcat (the runtime's tag BOTWRecompAndroid, and native
# crashes) for that long (default 60 s) into android/out/boot.log, then prints its tail.
set -euo pipefail
export MSYS_NO_PATHCONV=1  # (Git Bash: leave the device paths alone)
cd "$(dirname "$0")/.."
SECS=${1:-60}
ENV=LARECOMP_NO_LAUNCHER=1${2:+,$2}
OUT=android/out/boot.log
mkdir -p android/out
adb shell am force-stop com.zbotw.recomp
adb logcat -c
adb shell am start -n com.zbotw.recomp/com.zbotw.recomp.BOTWRecompAndroidActivity --es env "$ENV" >/dev/null
timeout "$SECS" adb logcat -v time BOTWRecompAndroid:V SDL:V DEBUG:V libc:F AndroidRuntime:E '*:S' > "$OUT" || true
grep -c . "$OUT" | sed 's/^/lines: /'
tail -40 "$OUT"
