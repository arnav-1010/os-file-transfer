// ============================================================
//  scheduler.cpp  –  Week 4 update: RWLock replaces plain mutex
// ============================================================
//  All queue/TCB mutations go through write_lock().
//  total_pending() uses read_lock() — multiple workers can call
//  it simultaneously without blocking each other.
// ============================================================

#include "../include/scheduler.h"
#include <iostream>
#include <chrono>

AMFLQScheduler::AMFLQScheduler() {}

// ── add_transfer (Writer) ─────────────────────────────────────────────────────
void AMFLQScheduler::add_transfer(std::shared_ptr<TCB> tcb) {
    {
        WriteGuard wg(rw_lock_);   // exclusive: modifying queues[]

        int q = tcb->priority_level - 1;
        if (q < 0)          q = 0;
        if (q >= NUM_QUEUES) q = NUM_QUEUES - 1;

        queues[q].push(tcb);
        std::cout << "[AMLFQ] Queued TCB #" << tcb->file_id
                  << " -> Queue " << (q + 1) << "\n";
        std::cout.flush();
    }
    // Notify next_transfer() that something is available
    cv_.notify_one();
}

// ── next_transfer (Writer — pops from queue) ─────────────────────────────────
std::shared_ptr<TCB> AMFLQScheduler::next_transfer() {
    // First, wait until at least one queue is non-empty.
    // We hold cv_mtx_ here (condition_variable requirement), NOT rw_lock_.
    std::unique_lock<std::mutex> cv_guard(cv_mtx_);
    cv_.wait(cv_guard, [this] {
        ReadGuard rg(rw_lock_);  // read-only probe
        for (int i = 0; i < NUM_QUEUES; i++)
            if (!queues[i].empty()) return true;
        return false;
    });

    // Now take write ownership to actually pop
    WriteGuard wg(rw_lock_);
    for (int i = 0; i < NUM_QUEUES; i++) {
        if (!queues[i].empty()) {
            auto tcb = queues[i].front();
            queues[i].pop();
            std::cout << "[AMLFQ] Dispatching TCB #" << tcb->file_id
                      << " from Queue " << (i + 1) << "\n";
            std::cout.flush();
            return tcb;
        }
    }
    return nullptr;
}

// ── requeue (Writer) ─────────────────────────────────────────────────────────
void AMFLQScheduler::requeue(std::shared_ptr<TCB> tcb, bool used_full_quantum) {
    {
        WriteGuard wg(rw_lock_);   // modifying queues[] and TCB priority_level

        if (used_full_quantum && tcb->priority_level < NUM_QUEUES)
            tcb->priority_level++;

        std::cout << "[AMLFQ] TCB #" << tcb->file_id
                  << (used_full_quantum ? " demoted" : " requeued")
                  << " to Queue " << tcb->priority_level << "\n";
        std::cout.flush();

        queues[tcb->priority_level - 1].push(tcb);
    }
    cv_.notify_one();
}

// ── apply_aging (Writer) ─────────────────────────────────────────────────────
void AMFLQScheduler::apply_aging() {
    WriteGuard wg(rw_lock_);  // promotes TCBs, modifies queues[]

    auto now = std::chrono::steady_clock::now();
    for (int i = 1; i < NUM_QUEUES; i++) {
        std::queue<std::shared_ptr<TCB>> new_q;
        while (!queues[i].empty()) {
            auto tcb = queues[i].front();
            queues[i].pop();

            double wait = std::chrono::duration<double>(
                              now - tcb->last_scheduled).count();

            if (wait > 10.0 && tcb->priority_level > 1) {
                tcb->priority_level--;
                std::cout << "[AMLFQ] AGING: TCB #" << tcb->file_id
                          << " promoted to Queue " << tcb->priority_level << "\n";
                std::cout.flush();
                queues[i - 1].push(tcb);
            } else {
                new_q.push(tcb);
            }
        }
        queues[i] = new_q;
    }
}

// ── total_pending (Reader) ────────────────────────────────────────────────────
int AMFLQScheduler::total_pending() {
    ReadGuard rg(rw_lock_);  // shared: multiple workers can call this concurrently
    int total = 0;
    for (int i = 0; i < NUM_QUEUES; i++)
        total += queues[i].size();
    return total;
}
