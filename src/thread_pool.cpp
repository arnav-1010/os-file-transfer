#include "../include/thread_pool.h"
#include <iostream>

ThreadPool::ThreadPool(int num_threads) : stop(false) {
    std::cout << "[ThreadPool] Starting " << num_threads << " worker threads\n";
    for (int i = 0; i < num_threads; i++)
        workers.emplace_back(&ThreadPool::worker_loop, this);
}

ThreadPool::~ThreadPool() {
    stop = true;
    condition.notify_all();
    for (auto& t : workers)
        if (t.joinable()) t.join();
    std::cout << "[ThreadPool] All workers stopped\n";
}

void ThreadPool::enqueue(std::function<void()> task) {
    { std::lock_guard<std::mutex> lock(queue_mutex); task_queue.push(task); }
    condition.notify_one();
}

int ThreadPool::pending_tasks() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    return task_queue.size();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            condition.wait(lock, [this] { return stop || !task_queue.empty(); });
            if (stop && task_queue.empty()) return;
            task = task_queue.front();
            task_queue.pop();
        }
        task();
    }
}
