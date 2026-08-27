# Event loop performance work

This document records a round of profiling-driven changes to the naemon event
loop, what was measured, what was changed and why, and what was deliberately
left alone. It exists so that the reasoning behind some slightly unusual code
is available to whoever touches it next.

Branch: `perf-experiments`. Tooling: `contrib/perfbench/`.

---

## Why the event loop is the thing to optimise

Naemon has two halves. The workers fork and exec plugins and scale across
cores. The event loop — scheduling, dispatching checks, reading results back,
writing state files — is a **single thread**. Whatever it costs per check is
therefore a hard ceiling on how large an installation can get, no matter how
many cores the machine has.

So the number to optimise is *main-thread CPU per check*, not wall clock.

## How it was measured

A synthetic install: 5 000 hosts × 20 services = 100 000 services on a 60 second
`check_interval`, sustaining about 1 760 active service checks per second. The
check command is `/bin/echo`, so plugin runtime is nearly zero and what remains
is naemon's own work.

Measurement is `utime+stime` of the main process from `/proc/<pid>/stat` over a
60 second window after a 40 second settle, divided by the number of jobs the
workers actually started in that window (`@wproc wpstats` via the query
handler). Both variants have to report the same check count, otherwise the
comparison is meaningless. Hot spots come from `perf record` with DWARF call
graphs on the same process.

See `contrib/perfbench/README.md` to reproduce.

### Caveats worth keeping in mind

- Synthetic load. A real installation also spends time in notifications,
  flapping, event handlers and broker modules, none of which this exercises.
- **No event broker module was loaded.** Most real installations run one
  (Statusengine, mod_gearman, livestatus). That changes the profile, and in
  particular it changes what the NEB early-out below is worth. Numbers here
  are a floor, not a prediction.
- The status file cost scales with *object count*; the check pipeline scales
  with *check rate*. A small installation sees a very different split.
- Measured under WSL2. Syscall and I/O costs differ from bare metal.

## Result

| Scenario | Before | After | Δ |
|---|---|---|---|
| Default config (`status_update_interval=10`) | 5.28 s CPU / 60 s | 2.76 s | −48 % |
| µs of loop CPU per check | 50.1 | 26.2 | −48 % |
| Check pipeline only (`status_file=/dev/null`) | 3.24 s | 1.93 s | −40 % |
| µs per check | 30.6 | 18.3 | −40 % |
| status.dat writer alone (derived) | 2.04 s | 0.83 s | −59 % |
| One retention.dat dump (117 MB file) | ~1.0 s | ~0.4 s | −60 % |

Extrapolated to a saturated core the ceiling moves from roughly 20 000 to
roughly 38 000 checks per second. That extrapolation is linear and therefore
optimistic; treat it as an order of magnitude.

---

## The changes

### 1. State file writers: no `fprintf()` per field

`src/naemon/nm_writebuf.{c,h}`, `xsddefault.c`, `xrddefault.c`

Both state files were written with one `fprintf()` per field — about 57 calls
per object in `status.dat`, 217 total in `xrddefault.c`. Every one of those has
to interpret its format string at runtime.

At 100 000 services with the default 10 second `status_update_interval` that is
roughly 570 000 format string parses per second. It was **51 % of the event
loop's CPU**, with 29 % of the total inside `__vfprintf_internal` alone. A
`fdopen()` stream also gets a 4 KB buffer, so a ~50 MB dump additionally cost
about 12 000 `write()` syscalls.

Both files are almost entirely integers and strings, which do not need a
format interpreter. They now go through a shared append buffer with typed
helpers. The header of `nm_writebuf.h` explains the mechanism in detail.

The exception is doubles. There are only about five per object, and
reproducing printf's rounding exactly is fiddly enough that getting it subtly
wrong would silently change the files. Those still go through `snprintf()`,
but their results are memoized on the double's bit pattern — across an object
list the same handful of values (`check_interval`, `0.000`) repeat constantly,
so the cache hits most of the time and the output is byte-for-byte what
`snprintf` produced.

`retention.dat` is written far less often (once per `retention_update_interval`
and at shutdown) but at 100k services it is ~117 MB, and the loop is blocked
for the whole dump. That is a scheduling stall, not a background cost: while it
runs, no check is dispatched and no result is read.

**Verified byte-identical** for both files, over full dumps including custom
variables, comments, downtimes and state history, and `retention.dat` still
reads back cleanly.

### 2. Check results no longer looked up by name

`checks_service.c`, `checks_host.c`

`process_check_result()` resolved the object a result belongs to with
`find_service(host_name, service_description)` — a GLib hash lookup plus a
string compare — although the code that dispatched the check had the pointer in
hand the whole time. 5.4 % of the loop. `handle_worker_host_check()` paid for it
twice, because it also looked the host up itself.

The object now travels to the result callback in a small job struct.

**Note for anyone extending this:** the pointer deliberately does *not* go into
`check_result`. That struct is part of the public API and event broker modules
populate it field by field, so a new member arrives uninitialised in results
submitted by a module. The first attempt did exactly that and crashed
`tests/test-scheduled-downtimes`, which builds one on the stack.

### 3. Event heap sort keys stored inline

`events.c`

The scheduling queue was an array of `timed_event *`, so every comparison while
sifting dereferenced two events to read their `event_time`. With 100 000
scheduled events that is 17 levels of essentially random memory access per heap
operation, and there are two heap operations per executed check.

The event time now sits inline next to the pointer as a nanosecond stamp, and
is compared as a single integer rather than a two-field `timespec`. About 40 %
off the heap's cost.

`tests/test-event-heap.c` reaches into the queue directly and was updated to
match.

### 4. Timeperiod day cache

`objects_timeperiod.c`

`check_time_against_period()` computed midnight with `localtime_r()`/`mktime()`,
and `_get_matching_timerange()` then computed the same midnight again — two
round trips per dispatched check, 3.4 % of the loop. Every caller asks about
"now", so the answer is nearly always the same between one check and the next.

The tricky part is that a local day is not always 86 400 seconds long and its
midnight is not always unambiguous — DST transitions, half-hour shifts, zones
whose transition happens *at* midnight so local 00:00 does not exist, leap
seconds under `right/` zones. Irregular days are therefore never cached and
fall through to the original computation. The comment block above the cache
explains the detection and the one non-obvious pitfall in it.

Down to 0.4 %.

**Testing.** `tests/test-timeperiod-daycache.c` compares the cache against a
reimplementation of the uncached computation over 1990–2036 in 16 timezones,
in sequential, random and *backwards* order (the last being what a system clock
stepped back looks like), plus a dense sweep across every DST transition and all
of February and March. It also pins down which days must and must not be
cached, and that a `tzset()` invalidates the cache.

The first version of this cache shipped two bugs that only a thorough test
would have caught, and both are now covered:

- a 22-hour validity window, which quietly stopped working after 22:00 local
  time;
- measuring the day length from a midnight that is itself DST-shifted, which
  made a transition day look exactly 86 400 seconds long. `t-tap/test_timeperiods`
  caught this one (Europe/London, 2009-10-25).

### 5. `iobroker_write_packet()` flushes one descriptor

`lib/iobroker.c`

It queued data for one descriptor and then called `iobroker_push()`, which walks
the entire descriptor set. The source already asked `/* horrible idea? */` about
it. Since dispatching a check goes through this path, every dispatched check
walked every registered worker socket.

It now flushes only the descriptor just written to. A backlog anywhere else is
still picked up by the `iobroker_push()` the event loop performs each iteration,
which is what that call is for.

### 6. NEB result set skipped when nothing is subscribed

`nebmods.c`, `broker.c`

`neb_make_callbacks()` allocated a `neb_cb_resultset` and a GLib pointer array,
iterated over an empty list, and tore both down. On an installation without
broker modules that is two allocations and two frees per call, and
`broker_service_check()` is called twice per executed check — plus it
`strdup()`s and tokenizes the command line to fill in event data nobody reads.

It now returns early when no callback is registered for that type. The return
code is unchanged; an empty result set yielded 0 before as well.

**This one only helps installations without a broker module**, which is not the
common case. With Statusengine or mod_gearman loaded the callbacks are real and
this early-out never fires.

### 7. `nsock_unix()` stack overflow (a real bug, unrelated to performance)

`lib/nsock.c`

Found while setting up the benchmark. `nsock_unix()` copied `strlen(path)` bytes
into `struct sockaddr_un.sun_path`, which is a fixed 108-byte array on Linux,
with no length check. Any `query_socket` path of 108 characters or more overran
the caller's stack frame — naemon died at startup with
`*** buffer overflow detected ***` inside `qh_init()`.

It now returns `NSOCK_EINVAL`. Every caller already handles a negative return,
so `qh_init()` reports a normal configuration error instead.

---

## Known bug found but deliberately not fixed

`xrddefault.c`, host block:

```c
nm_wb_kv_dbl(&wb, "normal_check_interval", "%f", temp_host->check_interval);
nm_wb_kv_dbl(&wb, "retry_check_interval",  "%f", temp_host->check_interval);   /* <- */
```

The second line writes `check_interval`, not `retry_interval`. The service block
a few dozen lines below gets it right, so this is a copy/paste slip rather than
a deliberate choice, and it predates this work.

Effect: a host whose `retry_interval` was changed at runtime (external command
`CHANGE_RETRY_HOST_CHECK_INTERVAL`, which is what sets
`MODATTR_RETRY_CHECK_INTERVAL`) has that change silently replaced by its normal
`check_interval` when the retention file is read back. Hosts whose
`retry_interval` was never modified are unaffected, because the reader only
applies the value when that attribute bit is set.

Left alone on purpose: correcting it changes what an existing `retention.dat`
means, so it wants its own patch and its own decision rather than riding along
with a performance change. There is a `FIXME` at the site.

---

## What is still hot

Shares of the *reduced* loop, so smaller absolute numbers than the same
percentages would have been before:

| Area | Share | Note |
|---|---|---|
| Macro expansion | ~8 % | `process_macros_r` builds its output quadratically: `strlen` + `realloc` + `strcat` per `$`-delimited segment. Next candidate. |
| Status writer | ~32 % | now split between the integer formatter and the five doubles per object |
| malloc churn | ~8 % | a check result, a job, several `strdup`s and a kvvec per check, all freed moments later |
| Event heap | ~4 % | |

One structural item not addressed: `event_poll_full()` runs **at most one timed
event per `epoll_wait()`**, and returns without running any when a descriptor
had input. At 1 760 checks/s the extra syscalls are noise. At 20 000 they would
not be.

## Things to be careful about when continuing

- `macros.h`, `checks.h` and `objects_*.h` are **installed public headers**.
  Livestatus and mod_gearman are still widely deployed and build against them.
  Internal rewrites are fine; signature and struct changes are not.
- Any change to the state file writers must be verified byte-identical against
  the previous implementation, not just "looks right". Diff a full dump with
  the genuinely time-dependent fields normalised.
- Benchmark runs are only comparable when both variants processed the same
  number of checks. Always interleave the variants.
