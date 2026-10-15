#include "job_manager.hpp"

// Job table + the state machine. Every change goes through transition(), which
// appends an event line, so events.csv is always a complete scheduling history.
#include <cstdio>
#include <sys/resource.h>
#include <sys/wait.h>

static bool legal(JobState from, JobState to) {
    switch (from) {
        case JobState::NEW:    return to == JobState::READY || to == JobState::KILLED;
        case JobState::READY:  return to == JobState::RUNNING || to == JobState::KILLED;
        case JobState::RUNNING:
            return to == JobState::READY || to == JobState::PAUSED ||
                   to == JobState::FINISHED || to == JobState::KILLED;
        case JobState::PAUSED: return to == JobState::READY || to == JobState::KILLED;
        default:               return false; // FINISHED / KILLED are final states
    }
}

JobManager::JobManager(CsvLogger& log, ProcMgr& proc, IScheduler& sched)
    : log_(log), proc_(proc), sched_(sched) {}

void JobManager::add_job(const std::string& name, double arrival, double burst_estimate,
                         int priority, const std::vector<std::string>& argv) {
    Job j;
    j.id = next_id_++;
    j.name = name;
    j.arrival = arrival;
    j.burst_estimate = burst_estimate;
    j.priority = priority;
    j.argv = argv;
    jobs_.push_back(j);
}

void JobManager::process_arrivals(double now) {
    for (Job& j : jobs_) {
        if (j.spawned || j.state != JobState::NEW || j.arrival > now) continue;
        j.pid = proc_.spawn(j.argv);
        j.spawned = true;
        if (j.pid > 0) {
            j.arrival_t = now; // observed arrival (spawn) time
            transition(j, JobState::READY, "arrive", now);
            sched_.on_arrive(&j);
        } else {
            transition(j, JobState::KILLED, "spawn_failed", now);
        }
    }
}

std::vector<Job*> JobManager::reap(double now) {
    std::vector<Job*> impacted;
    while (true) {
        int status = 0;
        rusage ru{};
        const pid_t p = wait4(-1, &status, WNOHANG, &ru);
        if (p <= 0) break; // 0 = children exist, none exited yet; -1 = no children
        Job* j = find_by_pid(p);
        if (!j) continue;
        // rusage gives the exact utime+stime at exit — better than the last
        // /proc sample (which can be up to one tick stale).
        j->m.cpu_time_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                          ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        if (j->state == JobState::RUNNING) {
            const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            transition(*j, ok ? JobState::FINISHED : JobState::KILLED,
                       ok ? "exit_ok" : "exit_fail", now);
            sched_.on_finished(j, true);
            impacted.push_back(j); // it was the RUNNING job -> clear in the caller
        }
        log_.job_done(*j);
    }
    return impacted;
}

void JobManager::dispatch(Job& j, double now) {
    if (j.state != JobState::READY) return;
    if (j.pid > 0 && !proc_.cont(j.pid)) return; // child died between ticks
    if (j.m.dispatch_count == 0) j.first_run_t = now;
    ++j.m.dispatch_count;
    transition(j, JobState::RUNNING, "dispatch", now);
}

bool JobManager::preempt(Job& j, double now) {
    if (j.state != JobState::RUNNING) return false;
    if (j.pid > 0 && !proc_.stop(j.pid)) return false; // died mid-preempt; reap finalises it
    transition(j, JobState::READY, "slice_expired", now);
    sched_.on_finished(&j, false); // RR puts it at the back of the queue
    return true;
}

bool JobManager::user_stop(int id, double now) {
    Job* j = find(id);
    if (!j || j->state != JobState::RUNNING) return false;
    if (j->pid > 0 && !proc_.stop(j->pid)) return false;
    transition(*j, JobState::PAUSED, "user_stop", now);
    return true;
}

bool JobManager::user_cont(int id, double now) {
    Job* j = find(id);
    if (!j || j->state != JobState::PAUSED) return false;
    // No SIGCONT here! In the 1-CPU model the job must go through the ready
    // queue and wait for a dispatch, otherwise two processes could run.
    transition(*j, JobState::READY, "user_cont", now);
    sched_.on_arrive(j);
    return true;
}

bool JobManager::user_kill(int id, double now) {
    Job* j = find(id);
    if (!j || j->state == JobState::NEW || j->state == JobState::FINISHED ||
        j->state == JobState::KILLED) return false;
    proc_.kill(j->pid);
    transition(*j, JobState::KILLED, "user_kill", now);
    sched_.on_killed(j); // scheduler drops it from its ready queue
    return true;
}

Job* JobManager::find(int id) {
    for (Job& j : jobs_) if (j.id == id) return &j;
    return nullptr;
}

Job* JobManager::find_by_pid(pid_t pid) {
    for (Job& j : jobs_) if (j.pid == pid) return &j;
    return nullptr;
}

bool JobManager::all_done() const {
    if (jobs_.empty()) return false; // wait for submissions, do not quit early
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
