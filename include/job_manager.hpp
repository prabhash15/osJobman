// Owns the job table and the state machine. All state transitions go through
// transition(), which logs every change, so events.csv is always complete.
#pragma once

#include "csv_logger.hpp"
#include "proc_mgr.hpp"
#include "scheduler.hpp"
#include "types.hpp"

#include <deque>
#include <string>
#include <sys/types.h>
#include <vector>

class JobManager {
public:
    JobManager(CsvLogger& log, ProcMgr& proc, IScheduler& sched);

    // Register a job (from the workload file or an interactive `submit`).
    void add_job(const std::string& name, double arrival, double burst_estimate,
                 int priority, const std::vector<std::string>& argv);

    // Spawn every NEW job whose arrival time has come; make it READY.
    void process_arrivals(double now);

    // Collect exited children (wait4/WNOHANG). Returns jobs whose exit ended
    // a RUNNING job, so the caller can clear its current-job pointer.
    std::vector<Job*> reap(double now);

    // Scheduler actions.
    void dispatch(Job& j, double now); // READY -> RUNNING
    bool preempt(Job& j, double now);  // RUNNING -> READY (slice expired)

    // User commands. Return false if the job or its state makes it a no-op.
    bool user_stop(int id, double now); // RUNNING -> PAUSED
    bool user_cont(int id, double now); // PAUSED -> READY (re-enters the queue)
    bool user_kill(int id, double now); // READY/RUNNING/PAUSED -> KILLED

    Job* find(int id);
    Job* find_by_pid(pid_t pid);
    std::deque<Job>& all() { return jobs_; }
    bool all_done() const;

    // Text table: id, name, state, timing columns and key metrics.
    void print_status(double now) const;

private:
    void transition(Job& j, JobState to, const std::string& event, double now);

    CsvLogger& log_;
    ProcMgr& proc_;
    IScheduler& sched_;
    std::deque<Job> jobs_; // deque => Job* stays stable while jobs are added
    int next_id_ = 1;
};
