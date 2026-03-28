#include "net/ThreadPool.hpp"
#include <stdexcept>

namespace net {

ThreadPool::ThreadPool(int num_threads) : m_num_threads(num_threads) {
    if (num_threads <= 0)
        throw std::runtime_error{"ThreadPool: num_threads must be > 0"};
}

void ThreadPool::start() {
    m_running = true;
    m_threads.reserve(m_num_threads);
    for (int i = 0; i < m_num_threads; ++i)
        m_threads.emplace_back([this]{ workerLoop(); });
}

void ThreadPool::stop() {
    {
        std::lock_guard lock(m_mutex);
        m_running = false;
    }
    m_cv.notify_all();
    for (auto& t : m_threads)
        if (t.joinable()) t.join();
    m_threads.clear();
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard lock(m_mutex);
        m_tasks.push(std::move(task));
    }
    m_cv.notify_one();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this]{
                return !m_tasks.empty() || !m_running;
            });

            if (!m_running && m_tasks.empty()) return;

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        task();
    }
}

ThreadPool::~ThreadPool() {
    if (m_running) stop();
}

}