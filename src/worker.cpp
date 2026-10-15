// Worker: the synthetic CPU-bound job.
//
//   worker <burst_seconds> <cpu_target_percent>
//
// Burns <burst_seconds> OF CPU TIME at roughly <cpu_target_percent> of one
// core (busy loop + small sleeps inside 50 ms slices). Budgeting uses
// CLOCK_PROCESS_CPUTIME_ID - process CPU time consumed - so a SIGSTOP freeze
// costs the job nothing (a wall clock cannot distinguish frozen time; CPU
// time can). Exits 0 when done.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

static double cpu_used() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: worker <burst_seconds> <cpu_target_percent>\n");
        return 2;
    }
    const double burst = std::atof(argv[1]);
    double target = std::atof(argv[2]);
    if (burst <= 0) return 0;
    if (target < 1) target = 1;
    if (target > 100) target = 100;
    const double on = target / 100; // fraction of a core while allowed to run
    const double STEP = 0.05;       // 50 ms slices

    volatile double spin = 1.234; // volatile: the loop may not be removed
    while (cpu_used() < burst) {
        // Busy phase: push process CPU time forward by STEP*on.
        const double goal = std::min(burst, cpu_used() + STEP * on);
        while (cpu_used() < goal) spin = spin + 0.137;
        // Idle phase: shape the CPU share toward the requested target.
        if (on < 1.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long long>(STEP * (1.0 - on) * 1000)));
    }
    (void)spin;
    return 0;
}
