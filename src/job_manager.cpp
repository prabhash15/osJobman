#include "job_manager.hpp"

// The job table, the state machine and the tick. Every state change goes
// through transition(), which appends one line to events.csv, so the log is
// always a complete and replayable scheduling history.
#include <cstdio>
#include <sys/resource.h>
#include <sys/wait.h>

// Which state changes are legal at all (everything else is refused).
static bool legal(JobState from, JobState to) {
    switch (from) {
        case JobState::NEW:    return to == JobState::READY || to == JobState::KILLED;
        case JobState::READY:  return to == JobState::RUNNING || to == JobState::KILLED;
        case JobState::RUNNING:
            return to == JobState::READY || to == JobState::PAUSED ||
                   to == JobState::FINISHED || to == JobState::KILLED;
        case JobState::PAUSED: return to == JobState::READY || to == JobState::KILLED;
        default:               return false; // FINISHED / KILLED are final
    }
}

JobManager::JobManager(CsvLogger& log, ProcMgr& proc, IScheduler& sched)
    : log_(log), proc_(proc), sched_(sched) {}

void JobManager::add_job(const std::string& name, double arrival, double burst,
                         int priority, const std::vector<std::string>& argv) {
    Job j;
    j.id = next_id_++;
    j.name = name;
    j.arrival = arrival;
    j.burst_estimate = burst;
    j.priority = priority;
    j.argv = argv;
    jobs_.push_back(j);
}

// --------------------------------------------------------------- the tick --
void JobManager::tick(double now, ProcSampler& sampler) {
    reap(now);
    process_arrivals(now);
    sample(sampler, now);
    maybe_preempt(now);
    dispatch_next(now);
}

// 1) Collect every child that exited since the last tick.
void JobManager::reap(double now) {
    while (true) {
        int status = 0;
        rusage ru = {};
        const pid_t pid = wait4(-1, &status, WNOHANG, &ru); // WNOHANG = never block
        if (pid <= 0) break; // 0 = children alive but none exited; -1 = no children
        Job* j = find_by_pid(pid);
        if (!j) continue;
        // The kernel reports the exact CPU time the child used (better than
        // the last /proc sample, which can be one tick old).
        j->m.cpu_time_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                          ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        if (j->id == running_id_) running_id_ = -1; // the CPU is free again
        if (j->state == JobState::RUNNING) {
            const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            transition(*j, ok ? JobState::FINISHED : JobState::KILLED,
                       ok ? "exit_ok" : "exit_fail", now);
        } else if (j->state != JobState::FINISHED && j->state != JobState::KILLED) {
            // A waiting job died without us (killed from outside).
            transition(*j, JobState::KILLED, "died", now);
            sched_.on_gone(j->id);
        }
        log_.job_done(*j);
    }
}

// 2) Spawn every job whose arrival time has come; it is born STOPPED.
void JobManager::process_arrivals(double now) {
    for (Job& j : jobs_) {
        if (j.state != JobState::NEW || j.arrival > now) continue;
        j.pid = proc_.spawn(j.argv);
        if (j.pid > 0) {
            j.arrival_t = now; // observed (not requested) arrival time
            transition(j, JobState::READY, "arrive", now);
            sched_.on_ready(j.id);
        } else {
            transition(j, JobState::KILLED, "spawn_failed", now);
        }
    }
}

// 3) Read /proc for the job that holds the CPU and log one sample.
void JobManager::sample(ProcSampler& sampler, double now) {
    if (running_id_ < 0) return;
    Job* j = find(running_id_);
    if (!sampler.sample(j->pid, j->m, now)) {
        reap(now); // it died between two ticks: reap() finishes the bookkeeping
        return;
    }
    log_.sample(now, j->id, j->m);
}

// 4) Ask the algorithm: has this job had the CPU long enough?
void JobManager::maybe_preempt(double now) {
    if (running_id_ < 0) return;
    if (!sched_.should_preempt(now - dispatch_t_)) return;
    preempt(now);
}

void JobManager::preempt(double now) {
    Job* j = find(running_id_);
    if (!proc_.stop(j->pid)) { reap(now); return; } // it died a moment ago
    running_id_ = -1;
    transition(*j, JobState::READY, "slice_expired", now);
    sched_.on_ready(j->id); // to the BACK of the ready queue
}

// 5) If the CPU is free, give it to the next ready job.
void JobManager::dispatch_next(double now) {
    if (running_id_ >= 0) return;          // the one CPU is busy
    const int id = sched_.pick_next();
    if (id < 0) return;                    // nobody is ready
    Job* j = find(id);
    if (j) dispatch(*j, now);
}

void JobManager::dispatch(Job& j, double now) {
    if (j.state != JobState::READY) return;      // stale queue entry, ignore
    if (j.pid > 0 && !proc_.cont(j.pid)) return; // died while stopped; reap() will see it
    if (j.m.dispatch_count == 0) j.first_run_t = now;
    ++j.m.dispatch_count;
    dispatch_t_ = now;
    running_id_ = j.id;
    transition(j, JobState::RUNNING, "dispatch", now);
}

// ------------------------------------------------------------ user commands
bool JobManager::user_stop(int id, double now) {
    Job* j = find(id);
    if (!j || j->state != JobState::RUNNING) return false; // only the running job
    if (!proc_.stop(j->pid)) return false;
    running_id_ = -1;
    transition(*j, JobState::PAUSED, "user_stop", now);
    return true;
}

bool JobManager::user_cont(int id, double now) {
    Job* j = find(id);
    if (!j || j->state != JobState::PAUSED) return false;
    // NO SIGCONT here: only one job may run at a time, so the job goes back
    // into the ready queue and gets its SIGCONT when it is dispatched.
    transition(*j, JobState::READY, "user_cont", now);
    sched_.on_ready(j->id);
    return true;
}

bool JobManager::user_kill(int id, double now) {
    Job* j = find(id);
    if (!j || j->state == JobState::NEW || j->state == JobState::FINISHED ||
        j->state == JobState::KILLED)
        return false;
    proc_.kill(j->pid); // SIGKILL (ProcMgr ignores an empty pid)
    if (j->id == running_id_) running_id_ = -1;
    transition(*j, JobState::KILLED, "user_kill", now);
    sched_.on_gone(j->id); // a dead job must never stay in the ready queue
    return true;
}

// ---------------------------------------------------------------- helpers --
Job* JobManager::find(int id) {
    for (Job& j : jobs_) if (j.id == id) return &j;
    return nullptr;
}

Job* JobManager::find_by_pid(pid_t pid) {
    for (Job& j : jobs_) if (j.pid == pid) return &j;
    return nullptr;
}

bool JobManager::all_done() const {
    if (jobs_.empty()) return false; // an empty run waits for submissions
    for (const Job& j : jobs_) {
        if (j.state != JobState::FINISHED && j.state != JobState::KILLED) return false;
    }
    return true;
}

void JobManager::print_status(double now) const {
    std::printf("t=%.1f  every job, live:\n", now);
    std::printf("  id name          state    arrived 1st_run  finish cpu_s  ctxNV disp\n");
    for (const Job& j : jobs_) {
        std::printf("  %2d %-13s %-8s %6.2f %6.2f %6.2f %6.2f %6ld %4d\n", j.id,
                    j.name.c_str(), state_name(j.state), j.arrival_t, j.first_run_t,
                    j.finish_t, j.m.cpu_time_s, j.m.ctx_nonvol, j.m.dispatch_count);
    }
}

// The one write path for job states: refuses illegal jumps, always logs.
void JobManager::transition(Job& j, JobState to, const std::string& event, double now) {
    if (!legal(j.state, to)) {
        std::fprintf(stderr, "warning: illegal transition %s -> %s (job %d) ignored\n",
                     state_name(j.state), state_name(to), j.id);
        return;
    }
    if (j.state == to) return;
    log_.event(now, j.id, j.state, to, event);
    j.state = to;
    if (to == JobState::FINISHED || to == JobState::KILLED) j.finish_t = now;
}

// Children never outlive the scheduler.
int JobManager::shutdown(double now) {
    int cancelled = 0;
    for (Job& j : jobs_) {
        if (j.state == JobState::FINISHED || j.state == JobState::KILLED) continue;
        if (j.pid > 0) proc_.kill(j.pid);
        transition(j, JobState::KILLED, "shutdown", now);
        sched_.on_gone(j.id);
        ++cancelled;
    }
    running_id_ = -1;
    while (wait(nullptr) > 0) {} // sweep: no zombie can survive wait()
    return cancelled;
}
