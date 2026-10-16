// monitor.hpp — read real numbers about a process from /proc. Pure file
// reading: no libraries, only the OS concept taught in the course.
#pragma once

#include "types.hpp"

class ProcSampler {
public:
    // Fills `out` (cpu time, cpu%, context switches) from /proc/<pid>/stat
    // and /proc/<pid>/status. Returns false when the process is gone.
    //
    // cpu_pct is averaged over a ~1 s window instead of one 100 ms tick:
    // the kernel counts CPU time in 10 ms steps, so a single tick would jump
    // between 0 and 100 for an ordinary job.
    bool sample(pid_t pid, Metrics& out, double now);

private:
    pid_t sampled_pid_ = -1; // whose window is being measured
    double window_t_ = 0;    // window start: wall seconds
    double window_cpu_ = 0;  // window start: CPU seconds
    double cpu_pct_ = 0;     // last computed percentage
};
