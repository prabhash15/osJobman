#include "monitor.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

// CPU time in /proc is counted in clock ticks (100 per second on Linux).
static double clock_ticks_per_second() {
    return double(sysconf(_SC_CLK_TCK));
}

// /proc/<pid>/stat looks like:  1234 (job name) S 1 2 ... utime stime ...
// The name may contain spaces and brackets, so only the text after the LAST
// ')' can be parsed field by field.
static bool read_cpu_ticks(pid_t pid, long& utime, long& stime) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    if (!f) return false;
    std::string line;
    std::getline(f, line);
    const std::string::size_type close = line.rfind(')');
    if (close == std::string::npos) return false;

    std::istringstream rest(line.substr(close + 1));
    std::vector<std::string> field;
    std::string tok;
    while (rest >> tok) field.push_back(tok);

    // field[0] is the state (file field 3), so utime (file field 14) sits at
    // index 11 and stime (field 15) at index 12.
    if (field.size() < 13) return false;
    utime = std::stol(field[11]);
    stime = std::stol(field[12]);
    return true;
}

// /proc/<pid>/status contains "key: value" lines, e.g.
// nonvoluntary_ctxt_switches: 12
static void read_context_switches(pid_t pid, long& vol, long& nonvol) {
    vol = nonvol = 0;
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key;
        long value = 0;
        if (!(ls >> key >> value)) continue;
        if (key == "voluntary_ctxt_switches:") vol = value;
        else if (key == "nonvoluntary_ctxt_switches:") nonvol = value;
    }
}

bool ProcSampler::sample(pid_t pid, Metrics& out, double now) {
    long utime = 0, stime = 0;
    if (!read_cpu_ticks(pid, utime, stime)) return false; // process is gone
    long vol = 0, nonvol = 0;
    read_context_switches(pid, vol, nonvol);

    const double cpu = double(utime + stime) / clock_ticks_per_second();

    if (pid != sampled_pid_) {          // a new process: open a new window
        sampled_pid_ = pid;
        window_t_ = now;
        window_cpu_ = cpu;
        cpu_pct_ = 0;
    } else {
        const double elapsed = now - window_t_;
        if (elapsed >= 1.0) {           // window reached 1 s: start a new one
            window_t_ = now;            // (the last percentage is kept for
            window_cpu_ = cpu;          //  this one tick, so it never blinks)
        } else if (elapsed > 0) {
            cpu_pct_ = std::min(100.0, 100.0 * (cpu - window_cpu_) / elapsed);
        }
    }

    out.cpu_pct = cpu_pct_;
    out.cpu_time_s = cpu;
    out.ctx_vol = vol;
    out.ctx_nonvol = nonvol;
    return true;
}
