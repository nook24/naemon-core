#!/bin/bash
#
# Measure how much CPU the naemon event loop burns under a synthetic load.
#
# The event loop is a single thread, so its CPU time per check is what limits
# how large an installation can get. This script starts one naemon, lets the
# schedule spread out, then samples utime+stime of the main process over a
# fixed window and divides by the number of checks the workers actually
# started in that window.
#
# Reporting checks alongside CPU is not optional: comparing two builds is only
# meaningful if they did the same amount of work, and a build that silently
# runs fewer checks would otherwise look faster.
#
# usage: measure.sh <naemon-binary-dir> <bench-dir> [seconds]
#
#   <naemon-binary-dir>  directory holding naemon and libnaemon.so*
#                        (LD_LIBRARY_PATH is set to it, which beats the
#                        RUNPATH baked in at configure time)
#   <bench-dir>          what gen-config.py produced
#
set -u

BINDIR=${1:?usage: measure.sh <naemon-binary-dir> <bench-dir> [seconds]}
BENCH=${2:?usage: measure.sh <naemon-binary-dir> <bench-dir> [seconds]}
DUR=${3:-60}
SETTLE=${SETTLE:-40}

CFG="$BENCH/naemon.cfg"
QH="$BENCH/var/naemon.qh"

# Kill by process *name*. A pkill -f pattern would also match this script and
# whatever shell invoked it, since their command lines mention these paths.
pkill -9 -x naemon 2>/dev/null
sleep 3

rm -f "$BENCH/var/naemon.log" "$BENCH/var/status.dat" \
      "$BENCH/var/naemon.pid" "$BENCH/var/out.log"

LD_LIBRARY_PATH="$BINDIR" setsid "$BINDIR/naemon" -d "$CFG" > "$BENCH/var/out.log" 2>&1

MAIN=""
for _ in $(seq 1 240); do
    sleep 1
    [ -s "$BENCH/var/naemon.pid" ] || continue
    MAIN=$(cat "$BENCH/var/naemon.pid")
    kill -0 "$MAIN" 2>/dev/null && break
    MAIN=""
done
if [ -z "$MAIN" ]; then
    echo "FAILED to start"
    tail -5 "$BENCH/var/out.log"
    exit 1
fi

# jobs_started, summed over all workers, via the query handler
jobs() {
    python3 - "$QH" <<'PY' 2>/dev/null | awk -F'jobs_started=' '{s+=$2} END{print s+0}'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3.0)
s.connect(sys.argv[1])
s.sendall(b"@wproc wpstats\0")
buf = b""
try:
    while True:
        d = s.recv(65536)
        if not d:
            break
        buf += d
except socket.timeout:
    pass          # the query handler keeps the connection open after replying
sys.stdout.write(buf.decode(errors="replace"))
PY
}

cpu() { awk '{print $14+$15}' "/proc/$MAIN/stat" 2>/dev/null; }

sleep "$SETTLE"

HZ=$(getconf CLK_TCK)
J0=$(jobs); T0=$(cpu); S0=$(date +%s.%N)
sleep "$DUR"
T1=$(cpu); S1=$(date +%s.%N); J1=$(jobs)

if [ -z "$T0" ] || [ -z "$T1" ]; then
    echo "naemon died during the run"
    tail -5 "$BENCH/var/out.log"
else
    python3 -c "
cpu = ($T1 - $T0) / $HZ
wall = $S1 - $S0
chk = $J1 - $J0
print('cpu=%6.2fs  core=%5.2f%%  checks=%-8d us_per_check=%5.1f' %
      (cpu, 100 * cpu / wall, chk, 1e6 * cpu / max(1, chk)))
"
fi

kill -TERM "$MAIN" 2>/dev/null
sleep 6
pkill -9 -x naemon 2>/dev/null
sleep 2
exit 0
