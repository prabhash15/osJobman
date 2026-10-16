// job_manager.hpp — the "mini-kernel": owns the job table, the state machine
// and the once-per-tick scheduling pass. Every state change goes through
// transition(), which logs it, so events.csv is always a complete history.
#pragma once

#include "csv_logger.hpp"
#include "monitor.hpp"
#include "proc_mgr.hpp"
#include "scheduler.hpp"
#include "types.hpp"

#include <string>
#include <sys/types.h>
#include <vector>

class JobManager {
public:
    JobManager(CsvLogger& log, ProcMgr& proc, IScheduler& sched);

    // Register a job (from the workload file or an interactive `submit`).
    void add_job(const std::string& name, double arrival, double burst,
                 int priority, const std::vector<std::string>& argv);

    // One scheduling pass, always in this order:
    //   1 reap   — collect children that exited since last tick
    //   2 arrive — spawn the jobs whose arrival time has come
    //   3 sample — read /proc for the job that holds the CPU
    //   4 preempt— ask the algorithm whether it should be stopped
    //   5 dispatch— hand the CPU to the next ready job
    void tick(double now, ProcSampler& sampler);

    // User commands. Return false when job/state makes it a no-op.
    bool user_stop(int id, double now); // RUNNING -> PAUSED
    bool user_cont(int id, double now); // PAUSED  -> READY  (back in the queue)
    bool user_kill(int id, double now); // anything -> KILLED

    bool all_done() const;
    void print_status(double now) const;

    // Kill and reap everything still alive; returns how many jobs it cancelled.
    int shutdown(double now);

private:
    // the five steps of tick()
    void reap(double now);
    void process_arrivals(double now);
    void sample(ProcSampler& sampler, double now);
    void maybe_preempt(double now);
    void dispatch_next(double now);

    void dispatch(Job& j, double now);  // READY   -> RUNNING
    void preempt(double now);           // RUNNING -> READY (slice expired)
    void transition(Job& j, JobState to, const std::string& event, double now);

    Job* find(int id);
    Job* find_by_pid(pid_t pid);

    CsvLogger& log_;
    ProcMgr& proc_;
    IScheduler& sched_;
    std::vector<Job> jobs_;
    int running_id_ = -1;    // the one job holding the CPU (-1 = CPU free)
    double dispatch_t_ = 0;  // when running_id_ got the CPU
    int next_id_ = 1;
};
