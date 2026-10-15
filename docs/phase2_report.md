# Phase 2 Report — OS-Level Resource Manager and Job Scheduler

Team: Ayush Srivastav (lead), Ayush Tiwari, Aman Rawat, Sarthak Bhatt,
Prabhash Uniyal, Chirag Dewrari.

> Skeleton for the Phase 2 deliverable (Sections 1-5 below are pre-written).
> Fill every ⟨MEASURE⟩ placeholder from the frozen run directories in `logs/`.

## 1. What Phase 2 delivered

- `jobman` + `worker` in plain C++17 (single `make`, `-Wall -Wextra` clean).
- Job management: `submit`, workload files, queues, full state machine.
- Process management: real children via `fork`/`exec`, `SIGSTOP`/`SIGCONT`
  preemption, `waitpid` reaping, `stop`/`cont`/`kill` commands.
- Scheduling: FCFS and Round Robin behind the `IScheduler` seam
  (SJF/Priority/MLFQ = Phase 3 drop-ins).
- Monitoring: `/proc/<pid>/stat` CPU time + CPU%, `/proc/<pid>/status`
  voluntary/nonvoluntary context switches; turnaround/waiting/response times.
- Logging: per-run `logs/run-*/{events,samples,metrics}.csv`.
- Verification: `tests/verify.py` (state machine, FCFS hand-check, RR
  fairness) + `tests/checklist.md`.

## 2. Simplifications (and why)

See `Phase2_Proposal.md` Section 2 and the README "Deviations" list: one
program instead of three components' worth of infrastructure; CSV instead of
a database; text status instead of a UI; 2 of 5 algorithms instead of 5.

## 3. Architecture snapshot

1-CPU model; children born stopped; 100 ms tick loop: reap → arrivals →
sample → (preempt?) → dispatch; single `IScheduler` seam; every transition
logged. (Diagram in `README.md`.)

## 4. Measured results ⟵ fill from logs/

Demo workload: `workloads/demo.csv` (7 jobs).

### FCFS run — ⟨MEASURE⟩: paste `metrics.csv` (logs/run-…fcfs…/)

- j1..j3 turnaround vs textbook (3.0/3.5/5.0 s): ⟨MEASURE⟩
- verification script verdict: ⟨MEASURE: paste verify.py output⟩

### RR run — ⟨MEASURE⟩

- j1..j3 first-run times vs FCFS: ⟨MEASURE⟩
- context switches observed: ⟨MEASURE: sum nonvoluntary from samples.csv⟩
- verification script verdict: ⟨MEASURE: paste verify.py output⟩

### Resource accounting sanity ⟵ checklist items 5-7 results

⟨MEASURE: worker 4 80 measured CPU% ≈ …; Ctrl-C shutdown row count …⟩

## 5. What remains for Phase 3

Algorithms (SJF, Priority, MLFQ) as new `IScheduler` classes; SQLite/CSV
loading + Python analysis plots; memory metrics via `/proc/<pid>/statm`;
experiment harness comparing algorithms over workloads; final report.
Nothing in Phase 2 needs rework for any of these (the seam and the CSVs are
the planned inputs).

## 6. Per-member what-I-built (fill before submission)

| Member | Built | Can explain live |
|---|---|---|
| Ayush Srivastav | ⟨fill⟩ | ⟨fill⟩ |
| Ayush Tiwari | ⟨fill⟩ | ⟨fill⟩ |
| Aman Rawat | ⟨fill⟩ | ⟨fill⟩ |
| Sarthak Bhatt | ⟨fill⟩ | ⟨fill⟩ |
| Prabhash Uniyal | ⟨fill⟩ | ⟨fill⟩ |
| Chirag Dewrari | ⟨fill⟩ | ⟨fill⟩ |
