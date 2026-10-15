// The single immutable seam of the design: Phase 3 adds SJF / Priority / MLFQ
// by implementing these six methods and changing nothing else.
#pragma once

#include "types.hpp"

#include <deque>
#include <string>

class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual std::string name() const = 0;
    virtual void on_arrive(Job* j) = 0;                   // job became READY
    virtual void on_finished(Job* j, bool naturally) = 0;  // running job left the CPU
    virtual void on_killed(Job* j) = 0;                    // job died while queued: forget it
    virtual Job* pick_next() = 0;                          // which job gets the CPU (nullptr = idle)
    virtual bool on_tick(const Job* running, double since_dispatch) = 0; // true = preempt
};

// First Come First Served: run in arrival order, never preempt.
class FcfsScheduler : public IScheduler {
public:
    std::string name() const override { return "FCFS"; }
    void on_arrive(Job* j) override;
    void on_finished(Job* j, bool naturally) override;
    void on_killed(Job* j) override;
    Job* pick_next() override;
    bool on_tick(const Job* running, double since_dispatch) override;

private:
    std::deque<Job*> ready_;
};

// Round Robin: fixed time slice, preempted jobs go to the back of the queue.
class RoundRobinScheduler : public IScheduler {
public:
    explicit RoundRobinScheduler(double quantum_s) : quantum_s_(quantum_s) {}
    std::string name() const override { return "RR"; }
    void on_arrive(Job* j) override;
    void on_finished(Job* j, bool naturally) override;
    void on_killed(Job* j) override;
    Job* pick_next() override;
    bool on_tick(const Job* running, double since_dispatch) override;

private:
    std::deque<Job*> ready_;
    double quantum_s_;
};
