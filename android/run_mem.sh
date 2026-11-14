#!/bin/bash
# android/run_mem.sh <tag> <seconds> [LARECOMP_X=1,...]: run_boot.sh on $ANDROID_SERIAL with the memory sampled every 10 s
# (the app's PSS / swap from dumpsys meminfo, the device's MemAvailable and swap use) into android/out/mem-<tag>.txt,
# and the run's log into android/out/boot-<tag>.log. A low-memory kill shows as the app's pid going away.
set -uo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
TAG=$1 SECS=$2 ENVS=${3:-}
MEM=android/out/mem-$TAG.txt
mkdir -p android/out
: > "$MEM"
bash android/run_boot.sh "$SECS" "$ENVS" > /dev/null 2>&1 &
RUN=$!
sleep 5
t=0
while kill -0 $RUN 2>/dev/null; do
    pid=$(adb shell pidof com.botwrecomp.android | tr -d '\r')
    avail=$(adb shell "grep -E 'MemAvailable|SwapFree|SwapTotal' /proc/meminfo" | awk '{printf "%s %d MB  ", $1, $2/1024}')
    if [ -n "$pid" ]; then
        pss=$(adb shell "dumpsys meminfo $pid" | tr -d '\r' | grep -E '^ *TOTAL PSS:|TOTAL SWAP|^ *TOTAL ' | head -2 | tr -s ' ' | tr '\n' '|')
    else
        pss="(not running)"
    fi
    flt=""
    [ -n "$pid" ] && flt=$(adb shell "cat /proc/$pid/stat" | awk '{print "minflt " $10 " majflt " $12 " utime " $14 " stime " $15}')
    echo "t=${t}s pid=$pid $avail $pss $flt" >> "$MEM"
    [ -n "$pid" ] && [ -n "${TOPH:-}" ] && adb shell "top -H -b -n1 -p $pid -m 8" | tr -d '\r' | tail -9 >> "$MEM"
    sleep 10
    t=$((t + 15))
done
wait $RUN
cp android/out/boot.log "android/out/boot-$TAG.log"
tail -25 "$MEM"
