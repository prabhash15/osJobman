#pragma once

#include <ctime>
#include <string>
#include <sys/types.h>
#include <vector>

enum class JobState { NEW, READY, RUNNING, PAUSED, FINISHED, KILLED };

inline const char* state_name(JobState s) {
    switch (s) {
        case JobState::NEW:     return "NEW";
        case JobState::READY:   return "READY";
        case JobState::RUNNING: return "RUNNING";
        case JobState::PAUSED:  return "PAUSED";
        case JobState::FINISHED:return "FINISHED";
        case JobState::KILLED:  return "KILLED";
    }
    return "?";
}

struct Metrics {
    double cpu_time_s = 0;  // CPU time of the job (exact via rusage on reap, else /proc)
    double cpu_pct = 0;     // observed over the last sample interval (0..100)
    long ctx_vol = 0;       // voluntary context switches (/proc)
    long ctx_nonvol = 0;    // nonvoluntary context switches (/proc)
    int dispatch_count = 0; // how many times the scheduler gave this job the CPU
};

struct Job {
    int id = 0;
    std::string name;
    std::vector<std::string> argv; // parsed command line, e.g. {./bin/worker, 2.5, 70}
    double arrival = 0;    // requested arrival, seconds after jobman start (0 = manual submit)
    double burst_estimate = 0; // requested seconds of CPU work
    int priority = 0;      // 1 (high) .. 10 (low) — unused by FCFS/RR, needed in Phase 3
    double arrival_t = 0;  // observed time the job actually arrived (spawned)
    double first_run_t = 0; // observed time of first dispatch (response time)
    double finish_t = 0;   // observed time of FINISHED/KILLED
    JobState state = JobState::NEW;
    pid_t pid = -1;
    bool spawned = false;
    Metrics m;
};
