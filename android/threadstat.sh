#!/system/bin/sh
# android/threadstat.sh <pid> [seconds]: (on the device) each thread's share of the interval on a CPU and waiting to run
# (runnable, not running: /proc/<pid>/task/<tid>/schedstat), its CPU affinity mask and last CPU. Push to /data/local/tmp.
pid=$1
secs=${2:-5}
cd /proc/$pid/task || exit 1
for t in *; do
    set -- $(cat $t/schedstat)
    echo "$t $1 $2" > /data/local/tmp/ts_$t
done
sleep $secs
for t in *; do
    [ -f /data/local/tmp/ts_$t ] || continue
    set -- $(cat /data/local/tmp/ts_$t)
    r0=$2 w0=$3
    set -- $(cat $t/schedstat)
    r=$(( ($1 - r0) / (secs * 10000000) ))
    w=$(( ($2 - w0) / (secs * 10000000) ))
    cpu=$(awk '{print $39}' $t/stat)
    aff=$(taskset -p $t 2>/dev/null | awk '{print $NF}')
    [ $r -ge 2 -o $w -ge 2 ] && echo "$t $(cat $t/comm) run ${r}% waiting-to-run ${w}% affinity $aff last-cpu $cpu"
    rm -f /data/local/tmp/ts_$t
done
