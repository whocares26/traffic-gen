#include "net/EventLoop.hpp"

namespace net {

EventLoop::EventLoop() :  m_running(false) {
    m_epoll_fd = epoll_create1(0);
    if (m_epoll_fd == -1) {
        throw std::system_error(errno, 
            std::generic_category(), "Failed to create epoll");
    }
    m_stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_stop_fd == -1) {
        close(m_epoll_fd);
        throw std::system_error(errno, 
            std::generic_category(), "Failed to create stop fd");
    }
    add_fd(m_stop_fd, EPOLLIN, [this](uint32_t events) {
        uint64_t dummy;
        read(m_stop_fd, &dummy, sizeof(dummy));
        m_running = false;
    });
}
EventLoop::~EventLoop() {
    if (m_epoll_fd != -1) {
        close(m_epoll_fd);
    }
    if (m_stop_fd != -1) {
        close(m_stop_fd);
    }
}

void EventLoop::add_fd(int fd, uint32_t events, Callback cb) {
    if (m_callbacks.find(fd) != m_callbacks.end()) {
        return;
    }
    epoll_event ev{};
    {
        ev.events = events;
        ev.data.fd = fd;
    }
    int ec = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    if (ec == -1) {
        throw std::system_error(errno, 
            std::generic_category(), "Failed to add fd to epoll");
    }
    m_callbacks[fd] = cb;
}

void EventLoop::remove_fd(int fd) {
    auto it = m_callbacks.find(fd);
    if (it == m_callbacks.end()) {
        return;
    }
    int ec = epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    if (ec == -1 && errno != ENOENT) {
        throw std::system_error(errno, 
            std::generic_category(), "Failed to delete fd from epoll");
    }
    m_callbacks.erase(it);
}

void EventLoop::update_fd(int fd, uint32_t events) {
    auto it = m_callbacks.find(fd);
    if (it == m_callbacks.end()) {
        return;
    }
    epoll_event ev{};
    {
        ev.events = events;
        ev.data.fd = fd;
    }
    int ec = epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    if (ec == -1) {
        throw std::system_error(errno, 
            std::generic_category(), "Failed to update fd using EPOLL_CTL_MOD");
    }
}

void EventLoop::run() {
    std::vector<epoll_event> events(1024);
    m_running = true;
    while (m_running) {
        int alive = epoll_wait(m_epoll_fd, events.data(), 1024, -1);
        if (alive == -1) {
            switch(errno) {
                case EINTR:
                    continue;
                    break;
                default: 
                    throw std::system_error(errno, 
                        std::generic_category(), "Error during epoll_wait()");
                    break;   
            }
        }
        if (alive > 0) {
            for (int i = 0; i < alive; i++) {
                int fd = events[i].data.fd;
                uint32_t flags = events[i].events;
                auto it = m_callbacks.find(fd);
                if (it != m_callbacks.end()) {
                    // Копия — колбэк может вызвать remove_fd(fd) для
                    // самого себя (типичный случай TcpConnection::handle_close);
                    // тогда std::function из it->second будет уничтожен
                    // прямо во время вызова, а копия на стеке нас спасает.
                    auto cb = it->second;
                    cb(flags);
                }
            }
        }
    }
}
void EventLoop::stop() {
    m_running = false;
    if (m_stop_fd != -1) {
        uint64_t signal = 1;
        write(m_stop_fd, &signal, sizeof(signal));
    }
}

}