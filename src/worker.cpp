// worker.cpp — the synthetic CPU-bound job, the only "load source".
//
//   worker <burst_seconds> <cpu_target_percent>
//
// Burns <burst_seconds> OF CPU TIME at roughly <cpu_target_percent> of one
// core: inside 50 ms slices it busy-loops for the wanted share and sleeps
// the rest away.
//
// The budget is measured with clock(), which counts CPU time, not wall time:
// a SIGSTOP freeze costs the job nothing (a stopped process does not consume
// CPU time), so the job really exits after doing `burst` seconds of work.
// Exit code 0 = done.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h> // usleep

// CPU seconds this process has used so far (0 while stopped).
static double cpu_seconds() {
    return double(clock()) / double(CLOCKS_PER_SEC);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: worker <burst_seconds> <cpu_target_percent>\n");
        return 2;
    }
    const double burst = std::atof(argv[1]);
    double target = std::atof(argv[2]);
    if (burst <= 0) return 0;
    target = std::max(1.0, std::min(100.0, target));

    const double on = target / 100.0;  // share of one core we want to use
    const double slice = 0.05;         // one slice = 50 ms of wall time

    volatile double spin = 1.234;      // volatile: the compiler may not drop it
    while (cpu_seconds() < burst) {
        // 1) busy phase: push our own CPU time forward by slice * on
        const double goal = std::min(burst, cpu_seconds() + slice * on);
        while (cpu_seconds() < goal) spin = spin + 0.137;
        // 2) idle phase: sleep away the rest of the slice (nothing to do at 100%)
        if (on < 1.0)
            usleep(static_cast<unsigned>(slice * (1.0 - on) * 1e6));
    }
    (void)spin;
    return 0;
}
