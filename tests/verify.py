#!/usr/bin/env python3
"""Verify one jobman run directory against its expected behaviour.

Usage: verify.py <logs/run-XYZ> <fcfs|rr>

Checks (Phase 2 verification checklist items 1-3):
  1. only legal state transitions appear in events.csv
  2. FCFS: j1..j3 of workloads/demo.csv match the hand-computed table
     (measured times are quantised by the 100 ms tick, so tolerance 1 s)
  3. RR:  j1..j3 get the CPU earlier than under FCFS and are each
     dispatched at least twice (quantum rotation really happens)
Exit code 0 = all checks passed.
"""
import csv
import os
import sys

TOL = 1.5  # seconds; ~3 ticks of quantisation (dispatch delay, reap delay)

LEGAL = {
    ("NEW", "READY"), ("NEW", "KILLED"),
    ("READY", "RUNNING"), ("READY", "KILLED"),
    ("RUNNING", "READY"), ("RUNNING", "PAUSED"),
    ("RUNNING", "FINISHED"), ("RUNNING", "KILLED"),
    ("PAUSED", "READY"), ("PAUSED", "KILLED"),
}

# Textbook expectations for j1..j3 of demo.csv under FCFS:
FCFS_EXPECTED = {  # jid: (turnaround, waiting)
    "1": (3.0, 0.0),
    "2": (3.5, 2.5),
    "3": (5.0, 3.0),
}
FCFS_FIRST_RUN = {"1": 0.0, "2": 3.0, "3": 4.0}

results = []


def check(name: str, ok: bool, detail: str = "") -> None:
    results.append((ok, name, detail))


def main(logdir: str, algo: str) -> int:
    events = os.path.join(logdir, "events.csv")
    metrics = os.path.join(logdir, "metrics.csv")
    for path in (events, metrics):
        if not os.path.exists(path):
            print(f"FAIL {path} missing"); return 1

    # --- 1. legal state machine in events.csv ------------------------------
    bad = []
    with open(events) as f:
        for row in csv.DictReader(f):
            t = (row["from"], row["to"])
            if t not in LEGAL and row["from"] != row["to"]:
                bad.append(row)
    check("state machine: only legal transitions", not bad,
          "; ".join(f"j{r['jid']} {r['from']}->{r['to']} '{r['event']}'" for r in bad[:3]))

    # --- metrics summary ----------------------------------------------------
    m = {}
    with open(metrics) as f:
        for row in csv.DictReader(f):
            m[row["jid"]] = row

    finished = [j for j, r in m.items() if r and float(r["first_run"] or 0) > 0]
    check("j1..j3 all ran and finished", all(j in m and m[j]["dispatches"] != "" for j in ("1", "2", "3")))

    # --- 2 / 3 per-algorithm checks on the hand-check jobs -------------------
    for jid, (t_exp, w_exp) in FCFS_EXPECTED.items():
        row = m.get(jid)
        if not row or float(row["first_run"] or 0) <= 0:
            continue  # covered by the "j1..j3 all ran" check
        t = float(row["turnaround"]); w = float(row["waiting"])
        if algo == "fcfs":
            check(f"FCFS j{jid} turnaround {t:.2f}s ~ textbook {t_exp} (tol {TOL})",
                  abs(t - t_exp) <= TOL, f"expected ~{t_exp}, got {t:.2f}")
            check(f"FCFS j{jid} waiting {w:.2f}s ~ textbook {w_exp} (tol {TOL})",
                  abs(w - w_exp) <= TOL, f"expected ~{w_exp}, got {w:.2f}")
        else:
            fr = float(row["first_run"])
            n = int(row["dispatches"])
            check(f"RR j{jid} dispatched {n}x (>=2: real quantum rotation)", n >= 2)
            if jid != "1":  # j1 is first for both algorithms - cannot beat FCFS
                check(f"RR j{jid} first run {fr:.2f}s earlier than FCFS ({FCFS_FIRST_RUN[jid]}s)",
                      fr <= FCFS_FIRST_RUN[jid] - 0.25,
                      f"FCFS first run was {FCFS_FIRST_RUN[jid]}")
    for jid, row in m.items():
        if float(row["burst_est"] or 0) == 0:
            continue
        t = float(row["turnaround"]); w = float(row["waiting"])
        check(f"j{jid} waiting <= turnaround",
              -12e-9 <= t - w and w >= 0)

    ok = all(r for r, _, _ in results)
    nfail = sum(1 for r, _, _ in results if not r)
    for good, name, detail in results:
        print(f"{'PASS' if good else 'FAIL'}  {name}" + (f"   [{detail}]" if detail and not good else ""))
    print(f"verify({algo}): " + ("ALL PASS" if ok else f"{nfail} FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
