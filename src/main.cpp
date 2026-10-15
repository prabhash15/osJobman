// jobman: the Phase 2 "mini-kernel" entry point.
//
// Usage:
//   jobman run [--algo fcfs|rr] [--quantum-ticks N] [<workload.csv>|-]
// (`-` or no file = interactive only; submit jobs from stdin instead)
//
// While running, type commands on stdin:
//   status | submit ./bin/worker 2 80 | stop ID | cont ID | kill ID | quit
//
// Model: a 1-CPU machine. At most one child process is RUNNING; it is
// preempted with SIGSTOP and the next job is dispatched with SIGCONT.
// Every tick (100 ms) makes one scheduling pass and logs to CSV.
#include "csv_logger.hpp"
#include "job_manager.hpp"
#include "monitor.hpp"
#include "proc_mgr.hpp"
#include "scheduler.hpp"
#include "types.hpp"

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
#include <sys/wait.h>
#include <vector>

static const double TICK_S = 0.1;  // scheduler decision granularity (100 ms)

// Monotonic seconds since process start. One clock for the whole program, so
// timestamps in the logs are comparable between jobs.
static double now_mono() {
    static struct timespec base = {};
    static bool init = true;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (init) { base = ts; init = false; }
    return double(ts.tv_sec - base.tv_sec) + double(ts.tv_nsec - base.tv_nsec) / 1e9;
}

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

static std::string make_run_dir() {
    ::mkdir("logs", 0755); // exists after the first call; failure is harmless
    char stamp[24];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "run-%Y%m%d-%H%M%S", std::localtime(&t));
    const std::string dir = std::string("logs/") + stamp;
    ::mkdir(dir.c_str(), 0755);
    return dir;
}

static void load_workload(JobManager& mgr, const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open workload file: %s\n", path.c_str());
        std::exit(1);
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto fields = split(line, ',');
        if (fields.size() != 5) continue;
        // name,arrival_s,burst_s,priority,cpu_target  ->  ./bin/worker burst target
        mgr.add_job(fields[0], std::atof(fields[1].c_str()), std::atof(fields[2].c_str()),
                    std::atoi(fields[3].c_str()),
                    {"./bin/worker", fields[2], fields[4]});
    }
}

// Call once per poll wake: pump all complete lines from stdin. Returns
// false only on `quit`. EOF simply stops further input.
static bool pump_stdin(JobManager& mgr, std::string& pending, double now) {
    char buf[512];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) return true;
    pending.append(buf, size_t(n));
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, pos);
        pending.erase(0, pos + 1);
        const auto words = split(line, ' ');
        if (words.empty()) continue;
        const std::string& cmd = words[0];
        const int id = words.size() > 1 ? std::atoi(words[1].c_str()) : 0;
        if (cmd == "quit") return false;
        if (cmd == "help") { usage(); continue; }
        if (cmd == "status") { mgr.print_status(now); continue; }
        if (cmd == "submit") {
            if (words.size() < 2) { std::printf("usage: submit ./bin/worker 2 80\n"); continue; }
            const auto argv = std::vector<std::string>(words.begin() + 1, words.end());
            const std::string program =
                argv[0].substr(argv[0].find_last_of('/') + 1); // table label only
            // Submitting the worker? infer the burst estimate from its arg.
            const double burst =
                (argv[0].find("worker") != std::string::npos && argv.size() > 1)
                    ? std::atof(argv[1].c_str()) : 0;
            mgr.add_job(program, now, burst, 5, argv);
            std::printf("job %s submitted\n", program.c_str());
            continue;
        }
        bool ok = false;
        if (cmd == "stop") ok = mgr.user_stop(id, now);
        else if (cmd == "cont") ok = mgr.user_cont(id, now);
        else if (cmd == "kill") ok = mgr.user_kill(id, now);
        else continue;
        std::printf("%s job %d: %s\n", cmd.c_str(), id, ok ? "ok" : "not allowed in this state");
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "run") {
        usage();
        const std::string first = argc >= 2 ? argv[1] : "";
        return (first == "help" || first == "-h") ? 0 : 1;
    }
    std::string algo = "rr";   // RR default: the tick clock makes rotation visible
    int quantum_ticks = 5;     // RR quantum default = 5 ticks = 500 ms
    std::string file = "-";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--algo" && i + 1 < argc) algo = argv[++i];
        else if (a == "--quantum-ticks" && i + 1 < argc) quantum_ticks = std::atoi(argv[++i]);
        else file = a; // workload file path, or '-' = none
    }
    if ((algo != "fcfs" && algo != "rr") || quantum_ticks < 1) { usage(); return 1; }

    const std::string logdir = make_run_dir();
    CsvLogger logger(logdir);
    ProcMgr proc;
    FcfsScheduler fcfs;
    RoundRobinScheduler rr(double(quantum_ticks) * TICK_S);
    IScheduler& sched = (algo == "fcfs") ? static_cast<IScheduler&>(fcfs)
                                         : static_cast<IScheduler&>(rr);
    JobManager mgr(logger, proc, sched);
    if (file != "-") load_workload(mgr, file);

    std::printf("algorithm=%s tick=%.1fs quantum_ticks=%d logs -> %s\n", sched.name().c_str(),
                TICK_S, quantum_ticks, logdir.c_str());
    std::signal(SIGINT, on_sigint);

    Job* running = nullptr;
    double dispatch_t = 0;
    bool keep = true;
    ProcSampler sampler;
    std::string pending;
    double next_tick = now_mono() + TICK_S;

    while (keep && !g_stop) {
        // Wait: until the next tick, or stdin wakes us earlier.
        struct pollfd pfd {STDIN_FILENO, POLLIN, 0};
        const double rem = next_tick - now_mono();
        const int r = ::poll(&pfd, 1, rem > 0 ? int(rem * 1000) : 0);
        if (r > 0 && (pfd.revents & POLLIN) && !pump_stdin(mgr, pending, now_mono())) keep = false;

        const double now = now_mono();
        if (now < next_tick || !keep) continue;
        next_tick += TICK_S;

        for (Job* j : mgr.reap(now)) // children dying frees the CPU slot
            if (j == running) running = nullptr;
        mgr.process_arrivals(now);

        if (running) {
            if (!sampler.sample(running->pid, running->m, now)) {
                running = nullptr; // died right after the last reap; next reap reports it
            } else {
                logger.sample(now, running->id, running->m);
                if (sched.on_tick(running, now - dispatch_t) && mgr.preempt(*running, now))
                    running = nullptr;
            }
        }
        if (!running) {
            Job* j = sched.pick_next();
            if (j) {
                mgr.dispatch(*j, now);
                running = j;
                dispatch_t = now;
            }
        }
        if (mgr.all_done()) keep = false;
    }

    // Shutdown: children never outlive the scheduler.
    const double end = now_mono();
    int cancelled = 0;
    for (Job& j : mgr.all()) {
        if (j.state == JobState::FINISHED || j.state == JobState::KILLED) continue;
        if (j.spawned) proc.kill(j.pid);
        logger.event(end, j.id, j.state, JobState::KILLED, "shutdown");
        j.state = JobState::KILLED;
        j.finish_t = end;
        ++cancelled;
    }
    while (wait(nullptr) > 0) {} // no zombies left behind
    std::printf("\ndone (%d job(s) cancelled at shutdown)\n", cancelled);
    mgr.print_status(end);
    std::printf("logs: %s/{events,samples,metrics}.csv\n", logdir.c_str());
    return 0;
}
