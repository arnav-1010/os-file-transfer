#include "../include/scheduler.h"
#include <iostream>
#include <chrono>

AMFLQScheduler::AMFLQScheduler() {}

void AMFLQScheduler::add_transfer(std::shared_ptr<TCB> tcb) {
    std::lock_guard<std::mutex> lock(mtx);
    int q = tcb->priority_level - 1;
    if (q < 0) q = 0;
    if (q >= NUM_QUEUES) q = NUM_QUEUES - 1;
    queues[q].push(tcb);
    std::cout << "[AMLFQ] Queued TCB #" << tcb->file_id
              << " -> Queue " << (q+1) << "\n";
    cv.notify_one();
}

std::shared_ptr<TCB> AMFLQScheduler::next_transfer() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [this] {
        for (int i = 0; i < NUM_QUEUES; i++)
            if (!queues[i].empty()) return true;
        return false;
    });

    // Pick from highest priority non-empty queue
    for (int i = 0; i < NUM_QUEUES; i++) {
        if (!queues[i].empty()) {
            auto tcb = queues[i].front();
            queues[i].pop();
            std::cout << "[AMLFQ] Dispatching TCB #" << tcb->file_id
                      << " from Queue " << (i+1) << "\n";
            return tcb;
        }
    }
    return nullptr;
}

void AMFLQScheduler::requeue(std::shared_ptr<TCB> tcb, bool used_full_quantum) {
    std::lock_guard<std::mutex> lock(mtx);

    if (used_full_quantum) {
        // Demote — used up its time slice, move to lower queue
        if (tcb->priority_level < NUM_QUEUES)
            tcb->priority_level++;
        std::cout << "[AMLFQ] TCB #" << tcb->file_id
                  << " demoted to Queue " << tcb->priority_level << "\n";
    }
    // else: paused/incomplete — stays at same priority

    int q = tcb->priority_level - 1;
    queues[q].push(tcb);
    cv.notify_one();
}

void AMFLQScheduler::apply_aging() {
    std::lock_guard<std::mutex> lock(mtx);
    auto now = std::chrono::steady_clock::now();

    // Promote transfers in lower queues that have been waiting too long (>10s)
    for (int i = 1; i < NUM_QUEUES; i++) {
        std::queue<std::shared_ptr<TCB>> new_q;
        while (!queues[i].empty()) {
            auto tcb = queues[i].front(); queues[i].pop();
            double wait = std::chrono::duration<double>(
                now - tcb->last_scheduled).count();
            if (wait > 10.0 && tcb->priority_level > 1) {
                tcb->priority_level--;
                std::cout << "[AMLFQ] AGING: TCB #" << tcb->file_id
                          << " promoted to Queue " << tcb->priority_level << "\n";
                queues[i-1].push(tcb);
            } else {
                new_q.push(tcb);
            }
        }
        queues[i] = new_q;
    }
}

int AMFLQScheduler::total_pending() {
    std::lock_guard<std::mutex> lock(mtx);
    int total = 0;
    for (int i = 0; i < NUM_QUEUES; i++) total += queues[i].size();
    return total;
}
