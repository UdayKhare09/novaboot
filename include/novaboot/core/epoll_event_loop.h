#pragma once

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include "novaboot/core/event_loop.h"

namespace novaboot::core {

/// Epoll-based EventLoop implementation.
///
/// Uses edge-triggered epoll for fd monitoring and a min-heap for timer
/// management. Timer deadlines are translated to epoll_wait timeouts.
class EpollEventLoop final : public EventLoop {
public:
    /// Max events to process per epoll_wait call
    static constexpr int kMaxEvents = 256;

    EpollEventLoop();
    ~EpollEventLoop() override;

    // Non-copyable, non-movable
    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    // ─── EventLoop interface ─────────────────────────────────────────
    void add_fd(int fd, std::uint32_t events, FdCallback cb) override;
    void modify_fd(int fd, std::uint32_t events) override;
    void remove_fd(int fd) override;

    TimerHandle add_timer(Duration delay, TimerCallback cb) override;
    TimerHandle add_timer_at(TimePoint when, TimerCallback cb) override;
    void cancel_timer(TimerHandle handle) override;

    void run() override;
    void run_once() override;
    void stop() override;

    [[nodiscard]] bool is_running() const noexcept override;
    [[nodiscard]] TimePoint now() const noexcept override;

private:
    struct FdEntry {
        int          fd;
        std::uint32_t events;
        FdCallback   callback;
    };

    struct TimerEntry {
        std::uint64_t id;
        TimePoint     deadline;
        TimerCallback callback;
        bool          cancelled = false;

        // Min-heap ordering: earlier deadlines first
        bool operator>(const TimerEntry& other) const noexcept {
            return deadline > other.deadline;
        }
    };

    /// Process all expired timers. Returns the time until the next timer.
    int process_timers();

    /// Process a single epoll iteration
    void process_events(int timeout_ms);

    int epoll_fd_ = -1;
    bool running_ = false;
    TimePoint cached_now_;

    // FD → callback mapping
    std::unordered_map<int, FdEntry> fd_entries_;

    // Timer min-heap
    using TimerQueue = std::priority_queue<
        TimerEntry,
        std::vector<TimerEntry>,
        std::greater<TimerEntry>>;
    TimerQueue timer_queue_;

    // Next timer ID (monotonically increasing)
    std::uint64_t next_timer_id_ = 1;

    // Set of cancelled timer IDs (for lazy deletion from the heap)
    std::unordered_map<std::uint64_t, bool> cancelled_timers_;
};

} // namespace novaboot::core
