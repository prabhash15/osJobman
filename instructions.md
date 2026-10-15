# instructions.md — os-jobsched, the complete explanation

> Goal: reading this single file should be enough to explain the whole project
> to a friend, a teammate, or the evaluation panel — the idea, every design
> decision with its reason, every module line by line, a second-by-second
> runtime walkthrough, how to run it, how to read every output, and answers
> to the questions evaluators ask.

---

## PART A — THE IDEA AND WHY EVERY DECISION WAS MADE

### A.1 The problem

Real operating systems must constantly decide *which process gets the CPU*.
Students learn the algorithms (FCFS, SJF, Priority, Round Robin, MLFQ) on
paper, but paper cannot show what actually happens: real context switches,
real preempted binaries, timing measured at kernel precision. This project
builds a small user-space "mini-kernel" — `jobman` — that manages **real
Linux child processes** like a CPU scheduler would, and measures everything.

### A.2 The core design decisions (each with its reason)

| # | Decision | Reason |
|---|----------|--------|
| 1 | **Model a 1-CPU machine**: at most one child process is ever RUNNING | One "CPU" makes scheduling unambiguous, makes textbook comparisons exact, and makes every algorithm change visible. Modern Linux hides the single-CPU abstraction behind many cores; we restore it at user level. |
| 2 | **Real processes, not simulation** | `fork()`/`exec()` a synthetic CPU-burner (`worker`) per job. Preemption is a real `SIGSTOP`; resumption is a real `SIGCONT`; the *kernel itself* counts the context switches we later show off. |
| 3 | **Children are born STOPPED** | A child must never run before the scheduler dispatches it (that would break decision #1). The child does `raise(SIGSTOP)` before exec; the parent waits until the stop *really happened* (`waitpid(WUNTRACED)`). No race, no uninvited CPU usage. |
| 4 | **A 100 ms tick drives everything** | One fixed heartbeat keeps the logic single-threaded and readable: reap → arrivals → sample → maybe preempt → dispatch next. The scheduler never needs a lock or a background thread (so the code needs no synchronization at all — a deliberate simplification). |
| 5 | **One immutable interface for algorithms** (`IScheduler`) | SJF/Priority/MLFQ (Phase 3) become new classes implementing 5 methods; nothing else in the codebase changes. This is the "seam" — the whole point of a well-structured project. |
| 6 | **Every state change is logged** | One private `transition()` function is the only write path for job state; it refuses illegal transitions and appends a line to `events.csv`. The CSV is therefore always a complete, auditable history — the grader can replay the run. |
| 7 | **CSV logs instead of a database (this phase)** | `events.csv` / `samples.csv` / `metrics.csv` need no server, no schema, no password; they load trivially into SQLite in Phase 3. Nothing thrown away. |
| 8 | **Measure CPU time from the kernel, not ourselves** | On exit we take the exact CPU time from kernel rusage (`wait4`); while running we read `/proc/<pid>/stat`. We never guess. |
| 9 | **Keep the code "one person can explain any file"** | 836 lines of C++ total (make builds exactly two binaries), no frameworks, no third-party libraries, textbook-only OS concepts. |

### A.3 The three rules of the system (memorize these)

1. **1-CPU model** — at most one RUNNING child.
2. **Born stopped** — a spawned child is SIGSTOPed before doing anything.
3. **One transition path** — state changes only through `transition()`, always logged.

---

## PART B — THEORY YOU MUST BE ABLE TO STATE

### B.1 The two implemented algorithms

**FCFS (First Come, First Served).** Jobs are served exactly in the order
they became ready. A job runs to completion; nothing preempts it.
- Fair and simple; can starve short jobs ("convoy effect": everyone waits
  behind one long job).

**Round Robin (RR).** Ready queue rotation with a fixed *time slice* (the
"quantum", here 500 ms = 5 ticks). The running job is preempted when its
quantum expires **and** someone else is ready; it goes to the *back* of the
queue. Short jobs finish fast; raw context-switch overhead is the cost —
which our logs show as `ctxNV` counters.

### B.2 The metric definitions (the math behind every number we report)

For one job:
| metric | formula | what it means |
|---|---|---|
| `response` | `first_run − arrived` | arrival→first taste of CPU (interactive feel: immediate service latency) |
| `turnaround` | `finished − arrived` | back-to-start delay for a user (total life in system) |
| `waiting` | `turnaround − cpu_time_s` | time spent in the READY/PAUSED states: turnaround minus the CPU time actually consumed. Algebraically equal to summing every not-running period, but computing it by subtraction can never drift from the other two numbers. |
| `cpu_time_s` | kernel `utime+stime` in seconds | how much CPU the job actually consumed |
| `cpu_pct` | rolling ~1 s share of one core | shape of the load (80% target vs 100% target) |
| `ctx_vol / ctx_nonvol` | `/proc/<pid>/status` counters | voluntary = process gave CPU up (sleep/yield/stop); nonvoluntary = scheduler took it (the interesting one for us) |

`dispatches` = how many times the scheduler gave this job the CPU. FCFS
always records 1. RR records ≥ number of slices the job needed — this count
*is* the RR lesson abstracted into a number.

Ordering rule checked everywhere: `0 ≤ waiting ≤ turnaround`.

### B.3 Why our measured numbers differ from textbook numbers

Tick quantisation: our system acts only on 100 ms boundaries, so arrival,
dispatch and completion can each be delayed by up to one tick — up to three
ticks of extra wait in the worst case for one job. Order never changes, only
absolute times shift; the checker (Section F) therefore compares against the
textbook with a ±1.5 s tolerance, which covers those three worst-case ticks.
That is the honest, defensible tolerance we state in the report.

---

## PART C — HOW THE SYSTEM WORKS, SECOND BY SECOND

### C.1 Start-up

1. `./bin/jobman run --algo rr workloads/demo.csv`
2. Argument parse: algorithm (`rr` default), quantum in ticks (5), workload file.
3. `logs/run-<timestamp>/` created; three CSVs opened with their header rows.
4. Workload parsed: each line becomes a `Job` record with `state=NEW`
   (not yet spawned), the worker command line, burst and arrival.
5. Banner printed; `SIGINT` handler registered; the clock and the tick
   deadline `next_tick = now + 0.1 s` are set.

### C.2 One tick (the heart of everything)

Wait via `poll()` until the next 100 ms boundary OR until stdin has a
command — whichever comes first, so typing never waits a full tick. Then:

```
1. REAP        wait4(-1, ..., WNOHANG, ...) (loop): gathers every child that
               exited since the last tick. rusage gives its exact CPU time.
               If it was the RUNNING job: state -> FINISHED (KILLED if exit
               code != 0), log exit_ok/exit_fail, scheduler told `naturally`
               (nothing re-queued) — CPU slot now free.
2. ARRIVALS    every NEW job whose arrival time has come: spawn (fork+exec,
               born stopped), log `arrive`, state NEW->READY, scheduler's
               on_arrive puts it in the ready queue.
3. SAMPLE      the RUNNING job's pid is read from /proc (cpu%, cpu time,
               switches). Row appended to samples.csv each tick.
4. MAYBE PREEMPT  ask the scheduler on_tick(running, wallSinceDispatch):
               RR says "preempt" when the quantum expired AND the ready
               queue has anyone else. If preempt: SIGSTOP the child,
               RUNNING->READY `slice_expired`, scheduler re-queues it.
5. DISPATCH    if the CPU is free: pick_next() from the ready queue.
               SIGCONT the child (first run of its life: record first_run_t,
               response starts here), dispatch_count++,
               READY->RUNNING `dispatch`.
6. ALL DONE?   when every job is FINISHED or KILLED, finish and print the
               final job table; log paths.
```

Note that *nothing* in this loop knows which algorithm is selected: the
decision "who runs next / preempt now?" is delegated to the scheduler object.
`jobman` is mechanics; the interface is policy.

### C.3 A concrete narrative: first 0.6 s of `--algo rr` on demo.csv

```
t=0.10  tick 1: j1 appears (spawned, born stopped), READY, ready queue [j1]
                dispatch: SIGCONT j1 -> RUNNING; j1 first_run = 0.10 s
t=0.50  tick 5: j2 appears (born stopped), READY, ready queue [j1, j2]
                j1 has consumed only 0.4 s of its 0.5 s quantum, so no
                preemption yet (someone IS waiting, but the quantum is not up)
t=0.60  tick 6: since_dispatch(j1) reached 0.5 s = quantum -> preempt:
                SIGSTOP j1 (the kernel counts 1 nonvoluntary switch),
                RUNNING -> READY, j1 goes to the BACK: queue [j2, j1],
                dispatch j2 -> RUNNING; j2 got its first run only 0.1 s
                after arriving (RR serves the short job immediately;
                under FCFS j2's first run would have been at 3.2 s)
```

From here the queue rotates: about every 500 ms one `slice_expired` event,
another ready job gets the CPU, `ctxNV` grows, and jobs exit exactly when
they have done their `burst` seconds of CPU work — measured, never estimated.

### C.4 Interactive commands (what each really does)

| command | state flow | signal actually sent |
|---|---------|----------------------|
| `submit ...` | NEW, arrival=now | (spawned next tick) |
| `stop 1` | RUNNING → PAUSED | SIGSTOP |
| `cont 1` | PAUSED → READY (requeued; no SIGCONT!) | SIGCONT only when later dispatched |
| `kill 1` | → KILLED | SIGKILL |
| `quit` | everything un-finished → KILLED | SIGKILL, then `wait()` sweeps |

The `cont` subtlety is important to explain: `cont` does **not** resume the
process immediately, because rule A.3-1 says only one process may run. The
paused job re-enters the ready queue and the scheduler dispatches it with a
SIGCONT at its turn. This is exactly what an OS does with a scheduled task
after a wait-state.

`kill` always allowed except for NEW (not yet spawned; there's nothing to
signal) and terminal states; killing also removes the job from the ready
queue via the interface's `on_killed` hook (a dead job must never occupy a
scheduler slot).

### C.5 Shutdown (three different doors)

All three will: kill any spawned-but-unfinished children (`SIGKILL`), reap
them with `wait` (safe sweep; no zombie can survive `wait(nullptr)`), log
each as `shutdown`, then print the final table:
1. `quit` typed by the user;
2. all jobs reached a terminal state (the run "naturally" ends);
3. Ctrl-C (`SIGINT`): a handler only sets a flag — no signal-unsafe work in
   the handler; the teardown runs from the main loop.

---

## PART D — MODULE BY MODULE, FILE BY FILE

```
include/types.hpp          Job, JobState, Metrics (pure data)
include/scheduler.hpp      IScheduler + FCFS/RR declarations
include/job_manager.hpp    JobManager: table+state machine+commands
include/proc_mgr.hpp       ProcMgr: fork/exec/stop/cont/kill
include/monitor.hpp        ProcSampler: /proc reader
include/csv_logger.hpp     CsvLogger: 3 CSVs per run
src/main.cpp               entry: args + tick loop + stdin + shutdown (+ the clock)
src/job_manager.cpp        implementation of the above table
src/schedulers.cpp         the two algorithms (15 lines each)
src/proc_mgr.cpp           fork/exec + signals
src/monitor.cpp            /proc/stat + /proc/status parsing
src/csv_logger.cpp         appends the three CSVs
src/worker.cpp             the synthetic CPU-burner (the only "load source")
workloads/demo.csv         shared 7-job workload (j1..j3 = checker anchors)
scripts/run_demo.sh        full demo: build → FCFS → RR → verify both
scripts/cleanup.sh         kills stray ./worker from crashed dev sessions
tests/verify.py            automated checks against textbook expectations
tests/checklist.md         human checklist (date/signature columns)
docs/phase2_report.md      report skeleton; fill ⟨MEASURE⟩ from logs/
Makefile                   two binaries, -std=c++17 -Wall -Wextra -O2
```

### D.1 `include/types.hpp` — the vocabulary

- `JobState`: NEW, READY, RUNNING, PAUSED, FINISHED, KILLED, with
  `state_name()` for printing. Every other file lives by these words.
- `Job`: identity (`id`, `name`, `argv`), request (`arrival`, `burst_estimate`,
  `priority`), observed timeline (`arrival_t`, `first_run_t`, `finish_t`),
  state, `pid`, `spawned` flag, and its `Metrics`.
- `Metrics`: `cpu_time_s` (kernel-reported), `cpu_pct` (rolling), `ctx_vol`,
  `ctx_nonvol`, `dispatch_count`.
- Deliberately no methods: state logic lives in JobManager only. One writer,
  zero surprises.
- Note `std::deque<Job>` in JobManager (not `std::vector`): deque never
  invalidates pointers when pushing back at the end, so `Job*` handed to the
  scheduler stays valid all run.

### D.2 `include/scheduler.hpp` + `src/schedulers.cpp` — the seam

This is the phase's best artifact: the whole algorithm world behind an
interface of five callbacks (all a Phase 3 class writes):

- `on_arrive(Job*)` — enqueue
- `on_finished(Job*, naturally)` — `false` → requeue (RR); `true` → drop (done)
- `on_killed(Job*)` — remove from the queue even if it died waiting
- `pick_next()` — who gets the CPU; `nullptr` = leave it idle
- `on_tick(running, since_dispatch)` — return `true` to preempt

**FCFS** — `ready_` queue, arrived order preserved, `pick_next` pops front,
`on_tick` returns false (never preempts). 15 lines.

**RR** — same queue discipline plus:
```cpp
bool RoundRobinScheduler::on_tick(const Job* running, double since_dispatch) {
    return running != nullptr && !ready_.empty() && since_dispatch >= quantum_s_;
}
```
Two subtleties that make it *correct* rather than merely plausible:
- `!ready_.empty()` — if nobody else wants the CPU, rotating is useless;
  we would stop/continue *the same process* and burn a context switch to
  achieve nothing (and 1 extra ctxNV per slice would pollute the data).
- requeue-to-back happens inside `on_finished(j, false)` — a preemption and
  a completion reach the scheduler through the same door, only the flag differs.

### D.3 `src/job_manager.cpp` — one table, one write path

- `add_job()` — registers a row; `arrival` is the trigger (seconds after
  start; interactive submits put `now`).
- `process_arrivals(now)` — spawns every NEW job whose time came,
  stamps `arrival_t`, logs `arrive`, enqueues it. Handles spawn failure
  (exec not found etc.) by KILLing the ghost row: logs, never crashes.
- `reap(now)` — the exit collector. `wait4(-1, ..., WNOHANG, &ru)` in a loop.
  Kernel rusage = the exact utime+stime at exit; a /proc sample could be up
  to one tick stale, rusage cannot. If the departing job was the RUNNING one
  it is reported back to the caller (frees the CPU slot).
- `dispatch`, `preempt` — the two scheduler actions, each: sanity state
  check → signal the child → `transition()` → scheduler callback.
- `user_stop/user_cont/user_kill` — the human-facing verbs, each a guarded
  state change (`user_cont` deliberately does NOT SIGCONT: see C.4).
- `all_done()` — run end condition; `jobs_.empty()` counts as "not done" so
  an interactive session waits for submissions rather than quitting at t=0.
- `print_status()` — the mini-`ps` table.
- `transition()` — the single write path (see A.3-3): refuses illegal jumps,
  writes `events.csv` via the logger, updates `state` and (for terminal
  states) `finish_t`.

The state machine in full (every legal edge):
```
NEW ──arrive──▶ READY ──dispatch──▶ RUNNING ──exit_ok───▶ FINISHED (terminal)
                 ▲  │                │  │
                 │  │ slice_expired  │  └─user_stop──▶ PAUSED
                 │  └────────────────┘                   │
                 │◀────────────── user_cont─────────────┘
                 (STATE FLOW SUMMARY: the dispatch halves JOIN the queue: PAUSED
                  re-enters as READY, so the CPU is never shared by two runs.)
The KILLED arms (all terminal): NEW→KILLED (spawn failure/shutdown-before-spawn),
and READY/RUNNING/PAUSED → KILLED via user_kill, shutdown (Ctrl-C/quit), or
exit_fail (non-zero exit, e.g. exec failure => _exit(127)).
illegal: NEW→RUNNING (never without a READY+dispatch),
         FINISHED/KILLED → anything (terminal states are final).
```

### D.4 `src/proc_mgr.cpp` — process control primitives

- `spawn(argv)`:
  1. `fork()`;
  2. child: `raise(SIGSTOP)` (becomes T (stopped) *immediately*), then
     `execvp()` the command — a stopped process survives exec, so the rule
     "no work before dispatched" is enforced by the kernel, not by us;
     if exec itself fails, the child `_exit(127)` — jobman sees `exit_fail`.
  3. parent: `waitpid(pid, WUNTRACED)` — returns exactly when the child
     *is stopped*; only then does spawn return. This single `WUNTRACED`
     line is what removes the classic fork/stop race.
- `cont/stop/kill(pid)`: one-line `kill(pid, SIGCONT|SIGSTOP|SIGKILL)`.

### D.5 `src/monitor.cpp` — reading the kernel truth

Two files, both standard:
- `/proc/<pid>/stat`: fixed-width integers EXCEPT the process-name field, so
  we skip to the *last* `)` before parsing: utime is field 14, stime 15
  (clock ticks; divide by `sysconf(_SC_CLK_TCK)` for seconds).
- `/proc/<pid>/status`: key: value lines `voluntary_ctxt_switches:`,
  `nonvoluntary_ctxt_switches:`.

`cpu_pct` uses a rolling 11-tick window (~1.1 s): instantaneous pcts from a
100 ms window would flip between 0 and 100 for a job that sleeps within itsslices; a full window smooths this to the honest average share. Bounds are
clamped to ≤100.

### D.6 `src/csv_logger.cpp` — persistence without a database


Constructor creates the three CSV files with their columns; three append
methods:
- `event(t, jid, from, to, note)` → `events.csv`
- `sample(t, jid, m)` → `samples.csv` (per tick, RUNNING only)
- `job_done(job)` → `metrics.csv` (turnaround/waiting/response maths live
  here: `turn = finish−arrived`; `wait = turn − cpu_time_s`;
  `resp = first_run−arrived`)

### D.7 `src/main.cpp` — the mini-kernel

Holds everything orchestration-ish:
- `now_mono()` — CLOCK_MONOTONIC seconds since process start (monotonic
  because wall clocks can jump; logs stay comparable).
- `usage()`, `split()` (CSV & space parsing), `make_run_dir()`,
  `load_workload()`, `pump_stdin()` (non-blocking stdin between ticks),
  `SIGINT` handler (flag set only).
- `main()` itself: parse args → open logger → choose scheduler object →
  build JobManager → load workload → banner → the tick loop (C.2) → shutdown
  (C.5).

**The `poll()` trick** (an evaluator favourite): handling input without a
second thread — `poll()` on stdin, timeout `next_tick − now` — returns when
either stdin has data or the tick is due, whichever first. If stdin wakes
first, all buffered lines are pumped immediately; a partial line survives in
a `pending` buffer until its newline arrives. EOF just stops new input; the
run continues (this is how the scripted demo pipes commands in).

### D.8 `src/worker.cpp` — where the CPU time truly goes

`./bin/worker <burst> <target_pct>`; 50 ms slices; within a slice, busy-spin
for `50ms × target%` then sleep the rest (2 ms naps). Two keys:
- Busy work is a `volatile` accumulator — the compiler may not remove it,
  so the loop really burns instructions rather than being optimised to
  nothing.
- The budget is `CLOCK_PROCESS_CPUTIME_ID` — *consumed CPU time* — not wall
  time: if the process is SIGSTOPed for 3 s in the middle, its budget does
  not evaporate (wall clocks cannot distinguish frozen time; CPU-time clocks
  freeze with the process). Therefore: exit ⇔ actually did `burst` seconds
  of CPU; measured `cpu_time_s` matches `burst_est` in every metrics row —
  a claim we can literally show the evaluator.

### D.9 `scripts/run_demo.sh`

`set -euo pipefail` (any failure aborts); `make`; run FCFS over the demo
workload; run RR over it; then verify the two freshest run directories with
`tests/verify.py` and print `DEMO OK` / non-zero exit. Demo + self-check are
one command — one exhibit for grading.

`scripts/cleanup.sh`: `pkill -x worker` — the **exact** process name, so it
cannot touch anything else.

---

## PART E — THE DATA FILES & THEIR GRAMMAR

### E.1 `workloads/demo.csv`

```
name,arrival_s,burst_s,priority,cpu_target
j1,0.0,3.0,2,100
j2,0.5,1.0,1,100
j3,1.0,2.0,3,100
j4,2.0,1.5,2,100
j5,3.0,2.5,1,80
j6,4.0,1.0,3,60
j7,5.0,2.0,2,100
```

(CAREFUL: the loader only skips lines that BEGIN with `#`; an inline comment
after the fields would make a line 9 fields wide and silently be skipped.)

Why exactly these seven: j1/j2/j3 (100% targets, early arrivals) are the
hand-check anchors for the FCFS table versus the textbook. j4 makes the RR
rotation richer (an extra contender from t=2.0). j5 and j6 have their CPU
share shaped below 100% — proving the scheduler + worker honestly limit CPU%
(j5's 2.5 s of CPU stretches to ~3.1 s of wall time). j7 closes the show.

### E.2 Each log's line formats (columns in exact order)

```
events.csv:  t, jid, from, to, event
  event ∈ arrive | dispatch | slice_expired | user_stop | user_cont |
          user_kill | exit_ok | exit_fail | spawn_failed | shutdown

samples.csv: t, jid, cpu_pct, cpu_time_s, ctx_vol, ctx_nonvol

metrics.csv: jid, name, arrived, first_run, finished, turnaround, waiting,
             response, burst_est, cpu_time_s, ctx_vol, ctx_nonvol, dispatches
```

Report-worthy observations to quote:
- In `metrics.csv`: for every 100%-target job, `burst_est == cpu_time_s`
  (the worker did exactly its promised work — the claim can be pointed at,
  not argued).
- Requested `arrival` (the workload value) appears only in the workload file;
  `metrics.csv` carries `arrived` — the OBSERVED spawn time, ≤1 tick later
  because spawning happens on the tick grid.
- In `events.csv`: under RR, `slice_expired` and `dispatch` events for
  j1..j3 interleave; under FCFS, `dispatch` appears exactly once per running
  job (no `slice_expired` at all).

---

## PART F — VERIFICATION: WHAT "WORKING" MEANS, MECHANICALLY

### F.1 `tests/verify.py <logs/run-XYZ> fcfs|rr`

Four battery categories, all from the CSVs (no simulation, no opinions):
1. State machine: every `events.csv` transition must be legal (or terminal).
2. FCFS anchors: j1/j2/j3 turnaround ≈ 3.0/3.5/5.0 and waiting ≈ 0/2.5/3.0
   (tolerance ±1.5 s for ≤3 ticks of quantisation). These are HAND-COMPUTED
   numbers (same formulas as B.2) — anyone can redo them on paper in five
   minutes, so the demo lands as "numbers match theory".
3. RR anchors: each of j1..j3 dispatched ≥2 times (rotation really happens),
   and j2/j3 get their FIRST run earlier than under FCFS (0.6 s and 1.7 s
   vs 3.2 s and 4.3 s in a real run — short jobs aren't stuck behind j1).
4. Per-row sanity: `0 ≤ waiting ≤ turnaround` for every finished job.
Exit 0 ⇔ ALL PASS. Both runs verified by `run_demo.sh` automatically.

### F.2 `tests/checklist.md` — the human side built on the proposal

stop/cont/kill live (with `ps` to prove nothing survives), resource
accounting (worker 4 80 measured at ~80% while the only RUNNING job), the
"one un-reaped child max" Ctrl-C case — each line gets date + who verified.

---

## PART G — RUNNING IT (REFERENCE)

```bash
cd os-jobsched
make                          # build both binaries
./scripts/run_demo.sh         # one-command full demo + verification
```

Workload mode:
```bash
./bin/jobman run --algo rr workloads/demo.csv
./bin/jobman run --algo fcfs --quantum-ticks 2 workloads/demo.csv
```
Interactive mode (`-` = no file; both modes may be mixed):
```bash
./bin/jobman run --algo rr
  submit ./bin/worker 2 80      # 2 s of CPU work at ~80% of one core
  status                        # job table (id, name, state, metrics)
  stop 1 | cont 1 | kill 1      # signal verbs (state rules apply)
  quit                          # graceful exit: children killed + reaped
```
Exit codes: `help`/`-h` 0 · bad usage / unknown algorithm 1 · clean run 0.

Single-job CPU-shape sanity: `submit ./bin/worker 4 80` as the only job →
`samples.csv` shows 80 ±1–2% throughout and `metrics.csv` shows
`cpu_time_s = 4.00` (exactly as promised, never estimated).

---

## PART H — EVALUATION QUICK ANSWERS

**"Why not just simulate jobs as sleep()-ing processes?"**
Because the point is the interaction between scheduler and reality:
real context switches (counted by the kernel), real exec, real rusage.
A sleep()-loop "job" can't be preempted (it's already asleep; our SIGSTOP
would move nothing), our CPU% measurement would be fiction.

**"Why is at most one job RUNNING? Isn't the machine multicore?"**
The scheduler we are studying models ONE cpu core (as taught: FCFS/RR/SJF/
MLFQ are single-processor algorithms). Multiplexing to N cores is Phase 3+
territory; starting with one core keeps every number checkable.

**"How do we know RR actually preempts?"**
`events.csv`: every quantum boundary has `slice_expired` → `dispatch` pairs;
`samples.csv`: ctx_nonvol grows across the interleaving; live: `status`
during a run flips which job is RUNNING every 500 ms.

**"What if a job is killed while queued?"**
`user_kill` → SIGKILL → the state machine marks KILLED AND the scheduler is
told via `on_killed` so the dead row is deleted from the ready queue —
otherwise `pick_next()` would later hand the CPU to a process that no longer
exists (dispatch would fail and silently lose the slot!).

**"What's the difference between `arrived` and `arrival`?"**
`arrival` = requested (the workload CSV says 0.5 s); `arrived` = observed
spawn time on the tick grid (the program was actually created at 0.6 s).
Printed in `metrics.csv` it captures real versus requested scheduling —
with the tick this is ≤0.1 s; the checker uses that.

**"Why 100 ms ticks and 500 ms quantum?"**
Sampling must be finer than decisions: with 100 ms ticks, a 5-tick quantum
is sampled 5 times inside one slice, so every preemption is observed on its
own sample (nothing slips between samples). Also, the tick grid bounds the
observable arrival/dispatch delay of ANY job by ≤0.1 s per step: measured-
vs-textbook error stays small and uniform (checker tolerance ±1.5 s).

**"What are Phase 3 plans?"** SJF, Priority, MLFQ as new `IScheduler`
classes; SQLite/CSV load + Python plots; memory metrics; experiments across
workloads; report writing. Zero changes to Phase 2 work needed — the seam
and the CSVs were built to be their inputs.

---

## PART I — PROVEN NUMBERS (reproducible any time via scripts/run_demo.sh)

Both verifications ALL PASS (`logs/run-…fcfs…`, `logs/run-…rr…`):

| check | measured | textbook |
|---|---|---|
| FCFS j1 turnaround / waiting | 3.10 / 0.10 s | 3.0 / 0.0 |
| FCFS j2 turnaround / waiting | 3.80 / 2.80 s | 3.5 / 2.5 |
| FCFS j3 turnaround / waiting | 5.40 / 3.40 s | 5.0 / 3.0 |
| RR j1 dispatched | 6× (FCFS: 1×) | ≥6 slices by arithmetic |
| RR j2/j3 first run | 0.6 / 1.7 s (FCFS: 3.2 / 4.3 s) | earlier than FCFS |
| `worker 4 80` CPU share | 81 % ±1 throughout | 80 % |
| `burst == cpu_time_s` for every 100% job | ✓ in `metrics.csv` | by construction |

---

## PART J — KNOWN LIMITATIONS (state them before someone finds them)

- Linux only (`/proc` + rusage); macOS/Windows are out of scope.
- Runs should happen on a quiet desktop. A background build will distort
  the measured turnarounds (we don't isolate cores).
- One jobman per run; `status` cannot be asked from a second shell — the
  state (and children!) live inside the running process (see README for the
  daemon-conversation we avoided on purpose).
- `cpu_pct` for a *queued* job is not sampled (only the RUNNING job's pid is
  sampled each tick) — a stop-paused job's counters don't move anyway, so
  there is nothing honest to report.
- Turnarounds include ≤~3 × 100 ms of tick quantisation on top of textbook;
  this is a real property of the implementation, not an error to hide.

---

## PART K — GLOSSARY (say these precisely)

- **QUANTUM / TIME SLICE** — max uninterrupted run per dispatch (RR).
- **READY vs RUNNING vs PAUSED** — READY means "wait for turn": in the
  kernel every non-running child of ours sits in state `T` (stopped) — both
  never-yet-dispatched children and slice-expired ones. PAUSED is our extra
  human word on top (only set by the `stop` command). RUNNING is the single
  SIGCONT-ed child.
- **CONTEXT SWITCH** — the kernel moving the CPU from one process to another;
  we produce them deliberately with SIGSTOP/SIGCONT and count them via
  `/proc/<pid>/status`.
- **RUSAGE** — per-process resource accounting the kernel keeps for every
  process (CPU time, page faults, ...); our highest-accuracy CPU reading,
  taken from `wait4()` at process exit.
- **ZOMBIE** — dead child not yet reaped by its parent; our shutdown `wait()`
  sweep makes them impossible to survive beyond a tick.
- **TIME QUANTISATION** — all our decisions happen on the 100 ms tick grid;
  timestamps in the logs are recorded at the moment they were observed, so a
  textbook re-calculation should be compared with up to three ticks of slack
  (see B.3).
