# os-jobsched — OS-Level Resource Manager and Job Scheduler (Phase 2)

Users submit computational jobs, the system runs them as **real Linux child
processes** (`fork`/`exec`, preempted with `SIGSTOP`/`SIGCONT`) and records
waiting/turnaround/response times, CPU% and context switches to CSV logs.

**Full guide with module explanations and worked examples: [instructions.md](instructions.md).**

## 60-second start

```bash
make                       # builds bin/jobman + bin/worker
./scripts/run_demo.sh      # FCFS run + RR run + automated textbook checks
```

Manual runs:

```bash
./bin/jobman run --algo rr workloads/demo.csv   # scripted workload
./bin/jobman run --algo rr                      # interactive; then type:
                                                # submit ./bin/worker 2 80
                                                # status | stop 1 | cont 1 | kill 1 | quit
```

Outputs land in `logs/run-<timestamp>/` (`events.csv` = state history,
`samples.csv` = per-tick metrics of the running job, `metrics.csv` = one
summary row per finished job). Checks: `python3 tests/verify.py
logs/run-XYZ fcfs` (or `rr`); manual cases in `tests/checklist.md`.

## Design in one page

- **1-CPU model**: at most one child is RUNNING; others rest stopped under
  `SIGSTOP`. Dispatching the next job is a *real* context switch we can count
  in `/proc/<pid>/status`.
- **Children are born stopped**: `worker` does `raise(SIGSTOP)` before exec;
  the parent waits for the stop (`WUNTRACED`). Nothing runs uninvited.
- **Tick loop (100 ms)**: reap exited children (`wait4/WNOHANG`, rusage gives
  the exact final CPU time) → spawn due arrivals → sample the running child
  from `/proc` → ask the algorithm whether to preempt → dispatch next job.
  RR quantum: 5 ticks = 500 ms (`--quantum-ticks N`).
- **Single seam**: adding SJF / Priority / MLFQ in Phase 3 = a new class
  implementing (`name`, `on_ready`, `on_gone`, `pick_next`, `should_preempt`)
  and nothing else (`include/scheduler.hpp`). The interface only ever passes
  plain `int` job ids — never a pointer.
- **Plain, university-level C++**: no owning pointers, no threads, no nested
  containers, no STL tricks — 930 lines total, about a third of them comments.
- **State machine**: NEW→READY→RUNNING→FINISHED with PAUSED/KILLED paths;
  every transition logged (`src/job_manager.cpp`).

```
user/stdin ─▶ JobManager ─▶ IScheduler (FCFS | RR)
                 │  spawn/stop/cont/kill (ProcMgr)
                 ▼
             real children (./bin/worker burst target)
                 ▲
             ProcSampler (/proc/<pid>/stat + status)
                 ▼
             CsvLogger (events / samples / metrics .csv)
```

Deviations from the proposal document (deliberate, recorded so nothing is silent):

- `submit` / `stop` / ... are **stdin commands of `jobman run`**, not separate
  shell invocations: state and children live in the running process, so a
  second invocation could not see them without a daemon.
- `status` prints at shutdown and on demand (stdin); a separate `jobman status`
  invocation is not possible for the same reason.
- `IScheduler` has `on_gone(id)` so a killed READY job cannot linger in
  the ready queue (correctness, not scope creep).
- The seam passes plain `int` job ids (not `Job*` as sketched in the
  proposal): identical behaviour, and no pointer can ever go stale — the
  whole of A.4-style plainness for the price of one `find(id)` call.

Layout and what to fill before submission see [instructions.md](instructions.md).
