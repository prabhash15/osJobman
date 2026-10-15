#!/usr/bin/env bash
# Reproducible Phase 2 demo: run FCFS, then RR, over the demo workload, then
# check both runs against the textbook expectations in tests/verify.py.
# CSV outputs stay in logs/run-*/ for the report.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "=== build ==="
make

echo; echo "=== run 1/2: FCFS ==="
./bin/jobman run --algo fcfs workloads/demo.csv < /dev/null

echo; echo "=== run 2/2: Round Robin ==="
./bin/jobman run --algo rr workloads/demo.csv < /dev/null

echo; echo "=== verify both runs ==="
mapfile -t dirs < <(ls -1d logs/run-* | sort)
if [ ${#dirs[@]} -lt 2 ]; then
  echo "expected two runs in logs/, found ${#dirs[@]}" >&2
  exit 1
fi
fails=0
# logs/run-* sorts by timestamp: second-newest is the FCFS run, newest is RR.
python3 tests/verify.py "${dirs[-2]}" fcfs || fails=$((fails+1))
python3 tests/verify.py "${dirs[-1]}" rr  || fails=$((fails+1))

echo
if [ "$fails" -eq 0 ]; then echo "DEMO OK - all checks passed"; else echo "DEMO FAILED"; fi
exit "$fails"
