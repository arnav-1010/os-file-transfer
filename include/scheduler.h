#pragma once
#include "tcb.h"
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>

#define NUM_QUEUES 3

// Time quanta per priority level (in chunks)
static const int QUEUE_QUANTUM[NUM_QUEUES] = {8, 16, 32};

class AMFLQScheduler {
public:
    AMFLQScheduler();

    void   add_transfer(std::shared_ptr<TCB> tcb);
    std::shared_ptr<TCB> next_transfer();   // blocking
    void   requeue(std::shared_ptr<TCB> tcb, bool used_full_quantum);
    void   apply_aging();                   // promote starving transfers
    int    total_pending();

private:
    std::queue<std::shared_ptr<TCB>> queues[NUM_QUEUES];
    std::mutex  mtx;
    std::condition_variable cv;
};
