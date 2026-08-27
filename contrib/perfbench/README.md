# Event loop benchmark

Tooling for measuring what the naemon event loop costs per check. The loop is a
single thread, so its CPU time per check is the ceiling on how large an
installation can get, and it is the number worth optimising against.

## What it measures

`measure.sh` samples `utime+stime` of the naemon main process over a fixed
window and divides by the number of jobs the workers started in that same
window (read from the query handler via `@wproc wpstats`).

Reporting both numbers matters. Comparing two builds is only meaningful when
they did the same amount of work — a build that quietly runs fewer checks
would otherwise look faster. Treat a run whose check counts differ by more
than about 1% as invalid.

## Setup

```bash
# 5000 hosts x 20 services = 100k services on a 60s interval
# -> about 1760 active service checks per second
contrib/perfbench/gen-config.py ~/.cache/naemon-perfbench/bench100k \
    --hosts 5000 --services 20
```

Then build the two variants you want to compare and copy each one's `naemon`
plus `libnaemon.so*` into its own directory:

```bash
./configure --prefix=$PWD/build && make -j && make install
mkdir -p ~/.cache/naemon-perfbench/v/opt
cp build/bin/naemon build/lib/libnaemon.so* ~/.cache/naemon-perfbench/v/opt/
```

A baseline is easiest to build in a worktree so the main tree stays untouched:

```bash
git worktree add /tmp/nbase master
cd /tmp/nbase && ./autogen.sh && ./configure --prefix=/tmp/nbase/build && \
    make -j && make install
mkdir -p ~/.cache/naemon-perfbench/v/base
cp build/bin/naemon build/lib/libnaemon.so* ~/.cache/naemon-perfbench/v/base/
```

`measure.sh` sets `LD_LIBRARY_PATH` to the binary directory, which takes
precedence over the `RUNPATH` baked in at configure time, so both variants can
live side by side regardless of what prefix they were configured with.

## Running

```bash
B=~/.cache/naemon-perfbench
for i in 1 2 3; do
    contrib/perfbench/measure.sh $B/v/base $B/bench100k 60
    contrib/perfbench/measure.sh $B/v/opt  $B/bench100k 60
done
```

Always interleave the variants rather than running all of one then all of the
other — machine state drifts, and interleaving keeps that drift from landing
entirely on one side. Three pairs is usually enough; within a series the spread
should be under a couple of percent.

## Isolating the two cost centres

The status file writer scales with *object count* and the check pipeline with
*check rate*, so they are worth measuring separately:

```bash
# pipeline only
sed -i 's|^status_file=.*|status_file=/dev/null|' $B/bench100k/naemon.cfg
```

## Profiling

`perf` is not installed on every machine and the packaged binary is tied to a
kernel version, but a mismatched `perf` is usually fine for user space
sampling:

```bash
apt-get download linux-tools-5.15.0-190-generic linux-tools-5.15.0-190
for d in *.deb; do dpkg-deb -x $d root/; done
PERF=root/usr/lib/linux-tools-5.15.0-190/perf

$PERF record -F 997 --call-graph dwarf,16384 -p $(cat $B/bench100k/var/naemon.pid) \
    -o perf.data -- sleep 40
$PERF report -i perf.data --children --stdio -g none --percent-limit 1.5
```

`--children` (inclusive time) is the useful view for finding which subsystem
costs what; `--no-children` (self time) tells you which function to actually
change. `/proc/sys/kernel/perf_event_paranoid` must be 2 or lower.

## Notes

- Keep the bench directory somewhere that survives a reboot. `/tmp` is cleared
  on many systems; `~/.cache` or a docker volume is safer.
- The default check command is `/bin/echo`, deliberately free of shell
  metacharacters so `runcmd` execs it directly instead of going through
  `/bin/sh`. Add metacharacters only if measuring that is the point.
- 100k services produce a ~50MB `status.dat` and a ~117MB `retention.dat`.
  Make sure the bench directory has room and is not on a filesystem whose
  `fsync` behaviour will dominate the measurement.
- With no event broker module loaded, the NEB callback paths are close to
  free. Real installations usually run one (Statusengine, mod_gearman,
  livestatus), which changes the profile — measure with the module you
  actually deploy.
