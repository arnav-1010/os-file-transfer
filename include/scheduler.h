#pragma once
// ============================================================
//  scheduler.h  –  Week 4 update: RWLock replaces plain mutex
// ============================================================
//  What changed from Week 3
//  ─────────────────────────
//  • Added #include "rw_lock.h"
//  • Replaced  std::mutex mtx  with  RWLock rw_lock_
//  • Kept      std::condition_variable cv  for blocking next_transfer()
//  • Worker threads that only READ queue sizes / TCB fields use read_lock()
//  • Scheduler thread that modifies queues[] / TCB uses write_lock()
//
//  Everything else (NUM_QUEUES, QUEUE_QUANTUM, AMFLQScheduler interface)
//  is identical to the Week 3 version — no callers need to change.
// ============================================================

#include "tcb.h"
#include "rw_lock.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>

#define NUM_QUEUES 3
static const int QUEUE_QUANTUM[NUM_QUEUES] = {8, 16, 32};

class AMFLQScheduler {
public:
    AMFLQScheduler();

    // Writer operations (modify queues[] or TCB priority_level)
    void add_transfer(std::shared_ptr<TCB> tcb);
    void requeue(std::shared_ptr<TCB> tcb, bool used_full_quantum);
    void apply_aging();

    // Blocking dispatch — wakes when any queue is non-empty
    std::shared_ptr<TCB> next_transfer();

    // Reader operation — only inspects queue sizes
    int total_pending();

private:
    std::queue<std::shared_ptr<TCB>> queues[NUM_QUEUES];

    // Week 4: RWLock protects queues[] and TCB priority_level
    //   Readers : worker threads calling total_pending()
    //   Writer  : scheduler thread calling add/requeue/apply_aging/next_transfer
    RWLock rw_lock_;

    // Condition variable still needed so next_transfer() can sleep
    // until work arrives without busy-waiting.
    // Paired with a plain mutex because std::condition_variable requires one.
    std::mutex              cv_mtx_;
    std::condition_variable cv_;
};
