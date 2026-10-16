// scheduler.hpp — the seam of the design. The algorithm only answers two
// questions ("which ready job runs next?" and "should the running one be
// stopped?"); jobman does all the real work. Phase 3 adds SJF / Priority /
// MLFQ by writing one new class here and changing nothing else.
//
// The scheduler never sees a Job or a pointer, only job ids (plain ints),
// so it can never do damage to the job table.
#pragma once

#include <string>
#include <vector>

class IScheduler {
public:
    virtual ~IScheduler() = default;

    virtual std::string name() const = 0;

    // A job became READY: put it in the ready queue.
    // (it arrived, it was resumed with `cont`, or it was preempted)
    virtual void on_ready(int id) = 0;

    // A job can never run again (killed): take it out of the queue.
    virtual void on_gone(int id) = 0;

    // Which ready job gets the CPU now? -1 = nobody wants it (leave it idle).
    virtual int pick_next() = 0;

    // The current job has held the CPU for `ran_s` seconds: preempt it?
    virtual bool should_preempt(double ran_s) = 0;
};

// First Come First Served: run in arrival order, never preempt.
class FcfsScheduler : public IScheduler {
public:
    std::string name() const override { return "FCFS"; }
    void on_ready(int id) override;
    void on_gone(int id) override;
    int pick_next() override;
    bool should_preempt(double ran_s) override;

private:
    std::vector<int> ready_; // job ids; front = next to run
};

// Round Robin: same queue, but a running job is preempted when its time
// slice (the quantum) is over and somebody else is waiting for the CPU.
class RoundRobinScheduler : public IScheduler {
public:
    explicit RoundRobinScheduler(double quantum_s) : quantum_s_(quantum_s) {}
    std::string name() const override { return "RR"; }
    void on_ready(int id) override;
    void on_gone(int id) override;
    int pick_next() override;
    bool should_preempt(double ran_s) override;

private:
    std::vector<int> ready_;
    double quantum_s_;
};
