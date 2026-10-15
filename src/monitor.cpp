#include "monitor.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

static bool read_stat(pid_t pid, long& utime, long& stime) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    if (!f) return false;
    // Field 2 (comm) may contain spaces, so skip to the last ')' first.
    std::string line;
    std::getline(f, line);
    auto close = line.rfind(')');
    if (close == std::string::npos) return false;
    std::istringstream rest(line.substr(close + 1));
    std::string tok;
    // After ')', fields start at the state (field 3): state=0, utime is
    // field 14 -> token 11 from here, stime (15) -> token 12.
    for (int i = 0; i < 11; ++i) {
        if (!(rest >> tok)) return false;
    }
    if (!(rest >> utime) || !(rest >> stime)) return false;
    return true;
}

static void read_status_switches(pid_t pid, long& vol, long& nonvol) {
    vol = nonvol = 0;
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key, value;
        if (!(ls >> key >> value)) continue;
        if (key == "voluntary_ctxt_switches:") vol = std::atol(value.c_str());
        if (key == "nonvoluntary_ctxt_switches:") nonvol = std::atol(value.c_str());
    }
}

static const int WINDOW = 11; // 11 ticks of 100 ms ~= 1 s of history

bool ProcSampler::sample(pid_t pid, Metrics& out, double now) {
    long utime = 0, stime = 0;
    if (!read_stat(pid, utime, stime)) return false; // process is gone
    long vol = 0, nonvol = 0;
    read_status_switches(pid, vol, nonvol);

    const double hz = double(sysconf(_SC_CLK_TCK));
    const double cpu_time = (utime + stime) / hz;

    auto& hist = hist_[pid]; // (wall, cpu) pairs, oldest first
    hist.emplace_back(now, cpu_time);
    if (hist.size() > size_t(WINDOW)) hist.pop_front();
    out.cpu_pct = 0;
    if (hist.size() == size_t(WINDOW)) { // full window -> steady average
        const auto& old = hist.front();
        const double wall = now - old.first;
        if (wall > 0)
            out.cpu_pct = std::min(100.0, 100.0 * (cpu_time - old.second) / wall);
    }

    out.cpu_time_s = cpu_time;
    out.ctx_vol = vol;
    out.ctx_nonvol = nonvol;
    return true;
}
