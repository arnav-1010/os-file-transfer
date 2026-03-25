#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

class ThreadPool {
public:
    ThreadPool(int num_threads);
    ~ThreadPool();
    void enqueue(std::function<void()> task);
    int  pending_tasks();

private:
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> task_queue;
    std::mutex                        queue_mutex;
    std::condition_variable           condition;
    std::atomic<bool>                 stop;

    void worker_loop();
};
