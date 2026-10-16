// types.hpp — the vocabulary of the program. Pure data, no logic: all the
// state logic lives in job_manager.cpp (one writer, zero surprises).
#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

// The life of a job. Only the arrows allowed by job_manager.cpp are legal.
enum class JobState { NEW, READY, RUNNING, PAUSED, FINISHED, KILLED };

inline const char* state_name(JobState s) {
    switch (s) {
        case JobState::NEW:      return "NEW";
        case JobState::READY:    return "READY";
        case JobState::RUNNING:  return "RUNNING";
        case JobState::PAUSED:   return "PAUSED";
        case JobState::FINISHED: return "FINISHED";
        case JobState::KILLED:   return "KILLED";
    }
    return "?";
}

// What the kernel tells us about the process of a job.
struct Metrics {
    double cpu_time_s = 0;  // CPU seconds really used (exact value from wait4)
    double cpu_pct = 0;     // share of one core over the last second (0..100)
    long ctx_vol = 0;       // voluntary context switches   (/proc/.../status)
    long ctx_nonvol = 0;    // involuntary context switches  (/proc/.../status)
    int dispatch_count = 0; // how often the scheduler gave this job the CPU
};

struct Job {
    int id = 0;                       // 1, 2, 3 ...
    std::string name;                 // shown by `status`
    std::vector<std::string> argv;    // command, e.g. {./bin/worker, 2.5, 70}
    double arrival = 0;               // requested arrival (seconds after start)
    double burst_estimate = 0;        // requested seconds of CPU work
    int priority = 0;                 // 1..10, unused by FCFS/RR (Phase 3)

    // Observed timeline in seconds after start; 0 = has not happened yet.
    double arrival_t = 0;   // when the process was really spawned
    double first_run_t = 0; // when it first got the CPU (response time starts)
    double finish_t = 0;    // when it reached FINISHED or KILLED

    JobState state = JobState::NEW;
    pid_t pid = -1;         // -1 = no process spawned yet
    Metrics m;
};
