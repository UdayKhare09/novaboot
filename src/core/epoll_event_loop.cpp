#include "novaboot/core/epoll_event_loop.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <sys/epoll.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace novaboot::core {

EpollEventLoop::EpollEventLoop() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::system_error(
            errno, std::system_category(), "epoll_create1 failed");
    }
    cached_now_ = Clock::now();
}

EpollEventLoop::~EpollEventLoop() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

void EpollEventLoop::add_fd(int fd, std::uint32_t events, FdCallback cb) {
    struct epoll_event ev{};
    ev.events  = events | EPOLLET; // Always edge-triggered
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::system_error(
            errno, std::system_category(), "epoll_ctl ADD failed");
    }

    fd_entries_[fd] = FdEntry{fd, events, std::move(cb)};
}

void EpollEventLoop::modify_fd(int fd, std::uint32_t events) {
    auto it = fd_entries_.find(fd);
    if (it == fd_entries_.end()) {
        throw std::runtime_error("modify_fd: fd not registered");
    }

    struct epoll_event ev{};
    ev.events  = events | EPOLLET;
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::system_error(
            errno, std::system_category(), "epoll_ctl MOD failed");
    }

    it->second.events = events;
}

void EpollEventLoop::remove_fd(int fd) {
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        // ENOENT is OK (fd may have been closed already)
        if (errno != ENOENT) {
            spdlog::warn("epoll_ctl DEL failed for fd {}: {}",
                         fd, std::strerror(errno));
        }
    }

    fd_entries_.erase(fd);
}

TimerHandle EpollEventLoop::add_timer(Duration delay, TimerCallback cb) {
    return add_timer_at(Clock::now() + delay, std::move(cb));
}

TimerHandle EpollEventLoop::add_timer_at(TimePoint when, TimerCallback cb) {
    auto id = next_timer_id_++;
    timer_queue_.push(TimerEntry{id, when, std::move(cb), false});
    return TimerHandle{id};
}

void EpollEventLoop::cancel_timer(TimerHandle handle) {
    if (handle) {
        cancelled_timers_[handle.id] = true;
    }
}

void EpollEventLoop::run() {
    running_ = true;

    while (running_) {
        run_once();
    }
}

void EpollEventLoop::run_once() {
    cached_now_ = Clock::now();

    // Process expired timers and get timeout for next one
    int timeout_ms = process_timers();

    // Wait for fd events (or timer expiry)
    process_events(timeout_ms);
}

void EpollEventLoop::stop() {
    running_ = false;
}

bool EpollEventLoop::is_running() const noexcept {
    return running_;
}

TimePoint EpollEventLoop::now() const noexcept {
    return cached_now_;
}

int EpollEventLoop::process_timers() {
    cached_now_ = Clock::now();

    while (!timer_queue_.empty()) {
        // Peek at the earliest timer
        // We need to use const_cast here because priority_queue::top()
        // returns a const reference, and we need to check/move the callback
        const auto& top = timer_queue_.top();

        // Check if this timer was cancelled
        auto cancel_it = cancelled_timers_.find(top.id);
        if (cancel_it != cancelled_timers_.end()) {
            cancelled_timers_.erase(cancel_it);
            // Remove the cancelled entry by popping it
            // We need a mutable reference, but priority_queue doesn't give us
            // one. The TimerCallback in `top` will be destroyed when we pop.
            timer_queue_.pop();
            continue;
        }

        if (top.deadline <= cached_now_) {
            // Timer has expired — fire it
            // Move the callback out before popping. This is technically UB
            // with std::priority_queue since top() is const, but we need to
            // move the callback. Use a workaround:
            auto entry = std::move(const_cast<TimerEntry&>(top));
            timer_queue_.pop();

            if (entry.callback) {
                entry.callback();
            }
        } else {
            // Earliest timer hasn't expired — calculate wait time
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                top.deadline - cached_now_);
            return static_cast<int>(std::max(wait.count(),
                                             std::chrono::milliseconds::rep{1}));
        }
    }

    // No timers — wait indefinitely (or up to 100ms for responsiveness)
    return 100;
}

void EpollEventLoop::process_events(int timeout_ms) {
    struct epoll_event events[kMaxEvents];

    int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);

    if (n < 0) {
        if (errno == EINTR) {
            return; // Interrupted by signal, retry
        }
        throw std::system_error(
            errno, std::system_category(), "epoll_wait failed");
    }

    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        auto it = fd_entries_.find(fd);

        if (it != fd_entries_.end() && it->second.callback) {
            it->second.callback(events[i].events);
        }
    }
}

} // namespace novaboot::core
