#include "scheduler.hpp"

#include <algorithm>

// C++17 std::erase: erase-remove idiom.
static void drop(std::deque<Job*>& queue, Job* j) {
    queue.erase(std::remove(queue.begin(), queue.end(), j), queue.end());
}

// ---------------------------------------------------------------- FCFS -----
void FcfsScheduler::on_arrive(Job* j) { ready_.push_back(j); }

void FcfsScheduler::on_finished(Job*, bool) {
    // FCFS never preempts; a finished job simply leaves the queue.
}

void FcfsScheduler::on_killed(Job* j) {
    drop(ready_, j);
}

Job* FcfsScheduler::pick_next() {
    if (ready_.empty()) return nullptr;
    Job* j = ready_.front();
    ready_.pop_front();
    return j;
}

bool FcfsScheduler::on_tick(const Job*, double) {
    return false; // no time slice, no preemption
}

// ------------------------------------------------------- Round Robin -------
void RoundRobinScheduler::on_arrive(Job* j) { ready_.push_back(j); }

void RoundRobinScheduler::on_finished(Job* j, bool naturally) {
    if (!naturally) ready_.push_back(j); // slice expired -> back of the queue
}

void RoundRobinScheduler::on_killed(Job* j) {
    drop(ready_, j);
}

Job* RoundRobinScheduler::pick_next() {
    if (ready_.empty()) return nullptr;
    Job* j = ready_.front();
    ready_.pop_front();
    return j;
}

bool RoundRobinScheduler::on_tick(const Job* running, double since_dispatch) {
    // Preempt only when the whole quantum passed AND someone else is waiting.
    return running != nullptr && !ready_.empty() && since_dispatch >= quantum_s_;
}
