#pragma once
#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace net {

class ThreadPool {
public:
    explicit ThreadPool(int num_threads);
    ~ThreadPool();

    void start();
    void stop();

    void submit(std::function<void()> task);

private:
    void workerLoop();

    const int                        m_num_threads;
    std::vector<std::thread>         m_threads;
    std::queue<std::function<void()>> m_tasks;
    std::mutex                       m_mutex;
    std::condition_variable          m_cv;
    std::atomic<bool>                m_running{false};
};

}