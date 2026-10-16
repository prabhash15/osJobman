// main.cpp — the entry point of jobman, the Phase 2 "mini-kernel".
//
// Usage:
//   jobman run [--algo fcfs|rr] [--quantum-ticks N] [<workload.csv>|-]
// (`-` or no file = interactive only; type the commands on stdin)
//
// While running:  status | submit ./bin/worker 2 80 | stop N | cont N | kill N | quit
//
// Model: a 1-CPU machine. At most one child process is RUNNING; it is
// preempted with SIGSTOP and the next job is dispatched with SIGCONT.
// A 100 ms tick drives everything and every state change is logged to CSV.
#include "csv_logger.hpp"
#include "job_manager.hpp"
#include "monitor.hpp"
#include "proc_mgr.hpp"
#include "scheduler.hpp"

#include <poll.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

static const double TICK_S = 0.1; // one tick = 100 ms

// Monotonic seconds since the program started. One clock for the whole
// program, so all timestamps in the logs are comparable (wall clocks can
// jump backwards, a monotonic clock cannot).
static double now_mono() {
    static struct timespec base = {};
    static bool first = true;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (first) { base = ts; first = false; }
    return double(ts.tv_sec - base.tv_sec) + double(ts.tv_nsec - base.tv_nsec) / 1e9;
}

// A signal handler may only set a flag: the teardown runs in main().
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

static void usage() {
    std::printf(
        "OS-Level Resource Manager and Job Scheduler - Phase 2\n"
        "usage: jobman run [--algo fcfs|rr] [--quantum-ticks N] [<workload.csv>|-]\n"
        "  workload line: name,arrival_s,burst_s,priority,cpu_target\n"
        "stdin commands while running:\n"
        "  status | submit ./bin/worker 2 80 | stop N | cont N | kill N | quit | help\n");
}

// Split a line into its fields, skipping empty ones.
static std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::istringstream ss(line);
    std::string item;
    while (std::getline(ss, item, sep)) {
        if (!item.empty() && item.find_first_not_of(" \t\r") != std::string::npos)
            out.push_back(item);
    }
    return out;
}

// logs/run-<timestamp>/ is created for every run.
static std::string make_run_dir() {
    ::mkdir("logs", 0755); // exists after the first run; failure is harmless
    char stamp[24];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "run-%Y%m%d-%H%M%S", std::localtime(&t));
    const std::string dir = std::string("logs/") + stamp;
    ::mkdir(dir.c_str(), 0755);
    return dir;
}

// One workload line: name,arrival_s,burst_s,priority,cpu_target
//   -> a job that runs  ./bin/worker <burst_s> <cpu_target>
static void load_workload(JobManager& mgr, const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open workload file: %s\n", path.c_str());
        std::exit(1);
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> fields = split(line, ',');
        if (fields.size() != 5) continue;
        mgr.add_job(fields[0], std::atof(fields[1].c_str()), std::atof(fields[2].c_str()),
                    std::atoi(fields[3].c_str()),
                    {"./bin/worker", fields[2], fields[4]});
    }
}

// One typed line -> one action. Returns false when the user types `quit`.
static bool run_command(JobManager& mgr, const std::string& line, double now) {
    const std::vector<std::string> w = split(line, ' ');
    if (w.empty()) return true;
    const std::string& cmd = w[0];
    const int id = w.size() > 1 ? std::atoi(w[1].c_str()) : 0;

    if (cmd == "quit") return false;
    if (cmd == "help") { usage(); return true; }
    if (cmd == "status") { mgr.print_status(now); return true; }

    if (cmd == "submit") {
        if (w.size() < 2) { std::printf("usage: submit ./bin/worker 2 80\n"); return true; }
        const std::vector<std::string> argv(w.begin() + 1, w.end());
        // name for the table = the program without its directory
        const std::string name = argv[0].substr(argv[0].find_last_of('/') + 1);
        // submitting the worker? then its burst is the first argument
        const double burst = (argv[0].find("worker") != std::string::npos && argv.size() > 1)
                                 ? std::atof(argv[1].c_str()) : 0.0;
        mgr.add_job(name, now, burst, 5, argv);
        std::printf("job %s submitted\n", name.c_str());
        return true;
    }

    bool ok = false;
    if (cmd == "stop") ok = mgr.user_stop(id, now);
    else if (cmd == "cont") ok = mgr.user_cont(id, now);
    else if (cmd == "kill") ok = mgr.user_kill(id, now);
    else return true; // unknown word: ignore it
    std::printf("%s job %d: %s\n", cmd.c_str(), id,
                ok ? "ok" : "not allowed in this state");
    return true;
}

// Read whatever the user has typed and run every complete line.
// Returns false on `quit`; sets `stdin_open` to false at EOF (the run then
// simply continues without any further commands).
static bool read_commands(JobManager& mgr, std::string& pending, double now,
                          bool& stdin_open) {
    char buf[512];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n == 0) { stdin_open = false; return true; } // EOF: no more input
    if (n < 0) return true;                          // nothing readable after all
    pending.append(buf, size_t(n));

    // A line may arrive in several reads, so keep the tail in `pending`.
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, pos);
        pending.erase(0, pos + 1);
        if (!run_command(mgr, line, now)) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    // ---- command line -------------------------------------------------
    if (argc < 2 || std::string(argv[1]) != "run") {
        usage();
        const std::string first = argc >= 2 ? argv[1] : "";
        return (first == "help" || first == "-h") ? 0 : 1;
    }
    std::string algo = "rr";  // default: Round Robin makes rotation visible
    int quantum_ticks = 5;    // default quantum: 5 ticks = 500 ms
    std::string file = "-";   // workload file, or '-' = interactive only
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--algo" && i + 1 < argc) algo = argv[++i];
        else if (a == "--quantum-ticks" && i + 1 < argc) quantum_ticks = std::atoi(argv[++i]);
        else file = a;
    }
    if ((algo != "fcfs" && algo != "rr") || quantum_ticks < 1) { usage(); return 1; }

    // ---- the pieces of the mini-kernel --------------------------------
    const std::string logdir = make_run_dir();
    CsvLogger logger(logdir);
    ProcMgr proc;
    ProcSampler sampler;
    FcfsScheduler fcfs;
    RoundRobinScheduler rr(double(quantum_ticks) * TICK_S);
    // Both objects live for the whole run; jobman only ever talks to them
    // through the IScheduler interface.
    IScheduler* chosen = &fcfs;
    if (algo == "rr") chosen = &rr;
    JobManager mgr(logger, proc, *chosen);
    if (file != "-") load_workload(mgr, file);

    std::printf("algorithm=%s tick=%.1fs quantum_ticks=%d logs -> %s\n",
                chosen->name().c_str(), TICK_S, quantum_ticks, logdir.c_str());
    std::signal(SIGINT, on_sigint);

    // ---- the loop: wait for a tick, then run exactly one tick ----------
    bool stdin_open = true;   // false after EOF (e.g. `jobman ... < /dev/null`)
    std::string pending;      // half-typed line, waiting for its newline
    double next_tick = now_mono() + TICK_S;

    while (!g_stop) {
        const double remaining = next_tick - now_mono();
        const int wait_ms = remaining > 0 ? int(remaining * 1000) : 0;

        if (stdin_open) {
            // Sleep until the next tick OR until the user types something,
            // whichever comes first (poll = "wake me on one of these events").
            struct pollfd pfd { STDIN_FILENO, POLLIN, 0 };
            if (::poll(&pfd, 1, wait_ms) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
                if (!read_commands(mgr, pending, now_mono(), stdin_open)) break; // quit
            }
        } else {
            ::usleep(unsigned(wait_ms) * 1000u); // no input can arrive any more
        }

        const double now = now_mono();
        if (now < next_tick) continue; // an early keystroke: keep waiting
        next_tick += TICK_S;
        mgr.tick(now, sampler);
        if (mgr.all_done()) break;     // every job is FINISHED or KILLED
    }

    // ---- shutdown: children never outlive the scheduler ---------------
    const double end = now_mono();
    const int cancelled = mgr.shutdown(end);
    std::printf("\ndone (%d job(s) cancelled at shutdown)\n", cancelled);
    mgr.print_status(end);
    std::printf("logs: %s/{events,samples,metrics}.csv\n", logdir.c_str());
    return 0;
}
