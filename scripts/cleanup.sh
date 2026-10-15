#!/usr/bin/env bash
# Kill leftover synthetic worker processes from a crashed jobman session.
# Matches the exact process name only - it cannot touch anything else.
set -u
pkill -x worker && echo "killed leftover worker(s)" || echo "no leftover workers found"
exit 0
