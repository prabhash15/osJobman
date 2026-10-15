# Phase 2 verification checklist

One row per check from the Phase 2 proposal (Section 8). Fill in date + who
verified. `./scripts/run_demo.sh` runs checks 1-3 automatically via
`tests/verify.py`.

| # | What to verify | How (command) | Expected | Date | Verified by |
|---|---|---|---|---|---|
| 1 | State-machine completeness | `python3 tests/verify.py logs/run-<fcfs-run> fcfs` | "state machine: only legal transitions" PASS | | |
| 2 | FCFS hand-check (j1..j3 waiting/turnaround vs hand calc) | `python3 tests/verify.py logs/run-<fcfs-run> fcfs` | PASS within ±1 s (tick quantisation) | | |
| 3 | RR fairness (earlier first runs, ≥2 dispatches each) | `python3 tests/verify.py logs/run-<rr-run> rr` | PASS | | |
| 4 | Preemption is real | RR run in terminal 1; in terminal 2 `grep slice_expired logs/run-<rr>/events.csv`; also `nonvoluntary` ctx switches in `samples.csv` grow | many slice_expired rows; ctxNV increases across j1..j3 | | |
| 5 | Kill/stop/cont | run `./bin/jobman run -`, submit, then `stop 1`, `cont 1`, `kill 2`; check `ps -ef \| grep worker` | states follow commands; no leftover processes; no zombies | | |
| 6 | Resource accounting sanity | `./bin/jobman run -` + `submit ./bin/worker 4 80` (the only running job); watch samples.csv cpu_pct and metrics.csv cpu_time_s | cpu_pct ≈ 80 ± a few % (rolling 1 s average); cpu_time_s = 4.00 at exit | | |
| 7 | Crash tolerance (graceful Ctrl-C) | Ctrl-C mid-run | remaining jobs logged as `shutdown` KILLED; `ps` shows no leftover children | | |
