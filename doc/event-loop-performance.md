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
- The headline numbers below were taken **without an event broker module**.
  Most real installations run one, and that does not just shift the numbers,
  it changes which code is worth optimising at all. There is a section on
  this further down; read it before drawing conclusions from the table.
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

## With an event broker module loaded, the picture changes completely

The numbers above describe naemon on its own. Measured again with the
Statusengine broker loaded (Gearman on localhost, worker processes draining
the queues so that a backed-up job server could not be mistaken for naemon
being slow), the same 100k service workload gives:

| | no broker | Statusengine loaded |
|---|---|---|
| loop CPU per check, before this work | 50.1 µs | 291.5 µs |
| loop CPU per check, after | 26.2 µs | 237.5 µs |
| share of one core at ~1760 checks/s, after | 4.2 % | 38.5 % |

The broker adds on the order of **210 µs per check**, which is roughly ten
times everything else in the event loop put together. `perf` on the main
thread, 36 000 samples:

```
84.7 %  neb_make_callbacks
 79.0 %    statusengine::Statusengine::Callback
   57.0 %      broker_service_status      (via update_service_status)
   27.5 %      broker_service_check
 20.7 %    json_object_to_json_string_length
 18.7 %  _int_malloc (self)               allocation churn out of json-c
```

Two things follow from this.

First, the work in this document is worth **−18 % per check** in a
Statusengine deployment, not the −48 % the standalone numbers suggest. Still
worth having, but the standalone figure is not the one to quote at anyone
running a broker.

Second, and more usefully: `broker_service_status` alone is 57 % of the loop,
and it fires **twice per check**.

- `checks_service.c`, in `schedule_next_service_check()` — because
  `next_check` and `check_options` were just updated. This runs at the *start*
  of every check, when `handle_service_check_event()` reschedules.
- `checks_service.c`, in `handle_async_service_check_result()` — when the
  result actually arrives.

Both carry the same `next_check`; the first differs from the second mostly by
the check's own runtime. So one full serialisation of the service object per
check is being produced for very little new information.

This is not new. It has been open as
[naemon/naemon-core#162](https://github.com/naemon/naemon-core/issues/162)
since 2016, and the Nagios bug tracker had it in 2009 before that. Until now a
module had no way to tell the duplicate apart from a real update, so the only
defence was to hash and checksum every event and drop the repeats — expensive,
and every module had to reinvent it.

How much this costs was measured by building a variant with that one call
removed entirely and running it against the same load:

| | µs of loop CPU per check | share of one core |
|---|---|---|
| current | 269.5 | 43.5 % |
| without the reschedule status event | 165.4 | 24.5 % |

**−39 % per check**, from a single line. For scale, that is more than the whole
of the rest of this document put together, and about thirty times what is left
to win in macro expansion (see below).

(The two variants processed check counts differing by 8 %, so the per-check
normalisation is doing real work here. Even taking the pessimistic reading —
scaling the cheaper variant up to the same check count — it lands at 8.75 s
against 14.24 s, which is the same conclusion.)

Removing the event is *not* what was done — see
[change 8](#8-a-separate-nebtype-for-schedule-only-status-events) below for
what was, and why.

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

The object now travels with the result, in a new `object_ptr` member of
`check_result`. `process_check_result()` uses it when it is set and falls back
to the lookup when it is NULL, so every existing caller keeps working.

**Why this took two attempts.** `check_result` is part of the public API and
modules populate it field by field, so a new member arrives holding whatever
was on the caller's stack — not NULL, which means a NULL check is no defence.
The first attempt did exactly that and crashed `tests/test-scheduled-downtimes`,
which builds one on the stack without zeroing it. The intermediate fix carried
the pointer past `check_result` in a private job struct instead.

What makes the field safe is `nebmods.c:197`: naemon refuses to load any module
whose `__neb_api_version` is not *exactly* `CURRENT_NEB_API_VERSION`. Bumping
that from 8 to 9 means a module compiled against the old struct cannot load at
all, so it can never submit a result with an uninitialised `object_ptr`. This
was verified by building a module against v8 and confirming the refusal:

```
Error: Module '...statusdiff_v8.so' is using an incompatible version (v8) of
the event broker API (current version: v9). Module will be unloaded.
```

`init_check_result()` was also changed to `memset()` the whole struct rather
than assign 18 fields individually — it had been quietly missing `output_file`,
`timeout` and `rusage`, which stayed as stack garbage even after the struct had
been "initialised". Removing that `memset` makes three of the result-processing
tests crash outright, which is the failure mode this guards against.

**What it is worth.** Active checks were already covered by the job struct this
replaces, so no large gain was expected here. Three interleaved pairs without a
broker, on identical check counts:

| | µs of loop CPU per check |
|---|---|
| job struct | 26.63 (26.0 / 26.0 / 27.9) |
| `object_ptr` | 26.07 (25.1 / 25.5 / 27.6) |

**−2.1 %**, negative in all three pairs (−0.9, −0.5, −0.3 µs). Smaller than the
run-to-run spread, so the paired deltas are what carry it, not the means — but
the sign is consistent and the mechanism is identifiable: the job struct cost
one `nm_calloc()` and one `nm_free()` per check, and those are gone.

The real point of the field is elsewhere: a broker module handing back a result
for an object it already tracks — mod_gearman is the obvious case — can now skip
the lookup too, which the job struct could never offer.

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

**This mostly helps installations without a broker module.** With Statusengine
or mod_gearman loaded, the callback types the module actually subscribes to do
real work and the early-out never fires for them; it still fires for the types
nothing is subscribed to, which is why it is not worthless there either.

### 7. `nsock_unix()` stack overflow (a real bug, unrelated to performance)

`lib/nsock.c`

Found while setting up the benchmark. `nsock_unix()` copied `strlen(path)` bytes
into `struct sockaddr_un.sun_path`, which is a fixed 108-byte array on Linux,
with no length check. Any `query_socket` path of 108 characters or more overran
the caller's stack frame — naemon died at startup with
`*** buffer overflow detected ***` inside `qh_init()`.

It now returns `NSOCK_EINVAL`. Every caller already handles a negative return,
so `qh_init()` reports a normal configuration error instead.

### 8. A separate NEBTYPE for schedule-only status events

`broker.h`, `checks_service.c`, `checks_host.c`

This addresses the duplicate status event described above, and closes
[naemon/naemon-core#162](https://github.com/naemon/naemon-core/issues/162).

Two new types:

```c
#define NEBTYPE_HOSTSTATUS_SCHEDULE              1204
#define NEBTYPE_SERVICESTATUS_SCHEDULE           1205
```

`schedule_next_service_check()` and `schedule_next_host_check()` now end with a
direct `broker_*_status(NEBTYPE_*STATUS_SCHEDULE, ...)` instead of
`update_*_status()`. That is the whole functional change — two lines.

`update_service_status()` and `update_host_status()` are untouched, and so are
their other ~28 and ~24 callers in `commands.c`, `downtime.c`, `flapping.c` and
`notifications.c`. Everything that represents a real change to the object still
arrives as `NEBTYPE_*STATUS_UPDATE`, exactly as before.

The split had to go on the *schedule* event rather than on the result event.
Tagging the result event instead would leave the schedule event as 1202, where
it is indistinguishable from a downtime, an acknowledgement or an external
command — a module would still have to receive and dedupe it, and the issue
would not be closed.

**Measured, not assumed.** `contrib/perfbench/statusdiff.c` is a diagnostic
broker module that snapshots every field the major brokers serialise and
reports which ones each NEBTYPE actually changes. Over a 5.5-minute run against
5 000 hosts and 100 000 services:

```
host NEBTYPE 1201: 10000 events, 0 (0.0%) changed nothing at all
    has_been_checked           30    0.3%
    last_check               5000   50.0%
    latency                  4745   47.5%
    execution_time           5000   50.0%
    last_time_up             5000   50.0%
    plugin_output              30    0.3%
    long_plugin_output       5000   50.0%
    perf_data                5000   50.0%
host NEBTYPE 1204: 10000 events, 0 (0.0%) changed nothing at all
    next_check              10000  100.0%
service NEBTYPE 1202: 599977 events, 0 (0.0%) changed nothing at all
    last_check             499977   83.3%
    latency                444402   74.1%
    execution_time         499954   83.3%
    last_time_ok           499977   83.3%
    long_plugin_output     100000   16.7%
    perf_data              100000   16.7%
service NEBTYPE 1205: 600000 events, 0 (0.0%) changed nothing at all
    next_check             600000  100.0%
```

`next_check` changes in 100 % of the schedule events, and in this run nothing
else did. That is **not** a general property, and a later run showed why.

Repeating the same measurement with checks distributed over mod_gearman instead
of run by naemon's own workers:

```
service NEBTYPE 1202: 599680 events, 54781 (9.1%) changed nothing at all
service NEBTYPE 1205: 654559 events, 0 (0.0%) changed nothing at all
    next_check             654559  100.0%
    last_check              54781    8.4%
    latency                 54781    8.4%
    execution_time          54702    8.4%
    check_options          109562   16.7%
    long_plugin_output      54781    8.4%
    perf_data               54781    8.4%
```

**8.4 % of the schedule events carry the fresh check result**, and the ratio is
1.09:1 rather than 1:1. Both come from the same mechanism:
`schedule_next_service_check()` also runs from inside
`handle_async_service_check_result()`, after the result has been written to the
object. The reschedule at dispatch (`checks_service.c:174`) is skipped while
`is_executing` is TRUE, which leaves `next_check_event` NULL, and the "make sure
a check is queued" branch (`:1164`) then does the rescheduling — at a point
where the object already holds the new state.

So the share is a property of the installation, not of the event type: it
measures how often a check is still outstanding when its next one falls due.
With sub-millisecond local checks it is 0 %, over a network 8.4 %, and with slow
checks it will be higher. The mirror image is the 9.1 % of *update* events that
change nothing any more, because the preceding schedule event already carried
the change.

None of this makes the event unsafe to drop: a full update always follows the
result, and every message is a complete snapshot rather than a delta.

(`check_options` and `last_update` are written too, but no major module
serialises them. `plugin_output` barely moves for services here because the
benchmark's check command returns constant output; that is a property of the
benchmark, not of the event.)

Two consequences a module author needs to know, both documented at the type
definitions in `broker.h`:

- **Store the second event's data.** The result event carries the fresh state
  *and* a `next_check` that is equal to or newer than the schedule event's —
  the retry reschedule (`checks_service.c:1045`) and the "make sure a check is
  queued" branch (`:1164`) both run *before* the status event on `:1169`.
  Nothing from the first event is lost.

- **The schedule event is not always redundant.** When a check is scheduled but
  then not run — host down, check period closed, dependencies or parents
  failed, checks disabled, result still inside the cache horizon, or
  `max_parallel_service_checks` reached — it is the *only* event that fires.
  A module that drops it outright will show a frozen `next_check` for exactly
  the objects that are not being checked. Handling it with a cheap
  next_check-only update is the accurate choice; returning early is the fast
  one.

**The saving lands in the module, not in the core.** naemon still emits the
event; what the split buys is the ability to not decode and forward it. A
module that ignores the type sees exactly what it saw before and is unaffected.
Of the three widely deployed modules, only Statusengine consumes these
callbacks at all — Livestatus and mod_gearman do not — and Statusengine's
`StandardCallback::Callback(int, void *)` currently discards the type argument,
so it needs a small patch to benefit.

That patch was written and measured. Three interleaved pairs, one and the same
naemon binary, the only difference being whether the module returns early on
`NEBTYPE_*STATUS_SCHEDULE`:

| | µs of loop CPU per check | share of one core |
|---|---|---|
| Statusengine as it is today | 265.1 (258.4 / 261.8 / 275.2) | 42.9 % |
| skipping the schedule event | 183.3 (177.4 / 186.7 / 185.9) | 29.7 % |

**−30.9 %.** The filtered variant processed 1.28 % *more* checks than the plain
one across the three pairs — a cheaper loop dispatches more work in the same
window — so the per-check normalisation is if anything understating the gain.

That is about four fifths of the −39 % that removing the call entirely
achieved. The remainder is the callback dispatch itself, which still runs:
`neb_make_callbacks()` walks its list and calls into the module, and only then
does the module return. Buying that last fifth would mean not emitting the
event, which costs `next_check` for skipped checks — a bad trade for 8 %.

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
| Macro expansion | ~8 % without a broker, **~2.4 % with one** | `process_macros_r` builds its output quadratically: `strlen` + `realloc` + `strcat` per `$`-delimited segment, and `clean_macro_chars()` allocates a copy of every macro value even when nothing needs stripping. Worth roughly 1 % of a broker-loaded loop, so not the next thing to do. |
| Status writer | ~32 % | now split between the integer formatter and the five doubles per object |
| malloc churn | ~8 % | a check result, a job, several `strdup`s and a kvvec per check, all freed moments later |
| Event heap | ~4 % | |

One structural item not addressed: `event_poll_full()` runs **at most one timed
event per `epoll_wait()`**, and returns without running any when a descriptor
had input. At 1 760 checks/s the extra syscalls are noise. At 20 000 they would
not be.

### What to do next, in order of value

For an installation running a broker module — which is most of them — the
ranking is not close:

1. **Patch the broker module to skip `NEBTYPE_*STATUS_SCHEDULE`.** −30.9 %,
   measured. The core side is done (change 8); the saving is only realised
   once the module stops decoding the event. This is a handful of lines in the
   module rather than a cross-project decision.
2. Everything else in this document. Already done, −18 % on top.
3. Macro expansion. ~1 %.

Optimising naemon further without doing (1) is polishing a part of the system
that is no longer where the time goes.

## Things to be careful about when continuing

- `macros.h`, `checks.h` and `objects_*.h` are **installed public headers**.
  Livestatus and mod_gearman are still widely deployed and build against them.
  Internal rewrites are always fine. A struct layout change is only safe
  together with a `CURRENT_NEB_API_VERSION` bump, which turns a silent memory
  bug in an unrebuilt module into a refusal to load; do not make one without
  the other. Changing the signature of an existing function has no such
  safety net — add a new function instead.
- Any change to the state file writers must be verified byte-identical against
  the previous implementation, not just "looks right". Diff a full dump with
  the genuinely time-dependent fields normalised.
- Benchmark runs are only comparable when both variants processed the same
  number of checks. Always interleave the variants.
