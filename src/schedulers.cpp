#include "scheduler.hpp"

// Two little queues of job ids. FCFS and RR differ ONLY in should_preempt().

// Take one job id out of the queue (does nothing when it is not there).
static void remove_id(std::vector<int>& queue, int id) {
    for (std::size_t i = 0; i < queue.size(); ++i) {
        if (queue[i] == id) {
            queue.erase(queue.begin() + i);
            return;
        }
    }
}

// Take the job at the front of the queue (the next one to run), or -1.
static int take_front(std::vector<int>& queue) {
    if (queue.empty()) return -1;
    const int id = queue.front();
    queue.erase(queue.begin());
    return id;
}

// ---------------------------------------------------------------- FCFS -----
void FcfsScheduler::on_ready(int id) { ready_.push_back(id); }

void FcfsScheduler::on_gone(int id) { remove_id(ready_, id); }

int FcfsScheduler::pick_next() { return take_front(ready_); }

bool FcfsScheduler::should_preempt(double /*ran_s*/) {
    return false; // no time slice in FCFS: a job runs until it is finished
}

// ----------------------------------------------------------- Round Robin ---
void RoundRobinScheduler::on_ready(int id) { ready_.push_back(id); }

void RoundRobinScheduler::on_gone(int id) { remove_id(ready_, id); }

int RoundRobinScheduler::pick_next() { return take_front(ready_); }

bool RoundRobinScheduler::should_preempt(double ran_s) {
    // Preempt only when the quantum is over AND somebody else is waiting.
    // Stopping a job while the queue is empty would only burn a context
    // switch to continue the very same process a moment later.
    return ran_s >= quantum_s_ && !ready_.empty();
}
