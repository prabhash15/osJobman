// Reads real process metrics from the /proc filesystem. Pure file reading —
// no libraries, exactly the OS-concept taught in the course.
#pragma once

#include "types.hpp"

#include <deque>
#include <map>
#include <utility>

class ProcSampler {
public:
    // Reads utime+stime from /proc/<pid>/stat and context switches from
    // /proc/<pid>/status. Fills `out` (cpu_time_s, cpu_pct, ctx counters).
    // cpu_pct is a rolling ~1 s average (single-tick windows would flip-flop
    // between 0 and 100 for a job that sleeps within its slice).
    // Returns false when the process no longer exists.
    bool sample(pid_t pid, Metrics& out, double now);

private:
    // (wall time, CPU time) pairs, oldest first, ~1 s of history per process.
    std::map<pid_t, std::deque<std::pair<double, double>>> hist_;
};
