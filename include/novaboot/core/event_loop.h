#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace novaboot::core {

/// Opaque handle for a registered timer.
struct TimerHandle {
    std::uint64_t id = 0;
    explicit operator bool() const noexcept { return id != 0; }
};

using Clock    = std::chrono::steady_clock;
using Duration = Clock::duration;
using TimePoint = Clock::time_point;

/// Backend selection for the event loop
enum class EventLoopBackend {
    Epoll,    ///< Traditional epoll (portable Linux 2.6+)
    IoUring,  ///< io_uring (Linux 5.19+, recommended)
};

/// Backend-agnostic event flags.
///
/// These match the EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP values used by both
/// epoll and io_uring's IORING_OP_POLL_ADD. No translation is needed
/// between backends.
namespace EventFlags {
    inline constexpr std::uint32_t Readable  = 0x001; // EPOLLIN  / POLLIN
    inline constexpr std::uint32_t Writable  = 0x004; // EPOLLOUT / POLLOUT
    inline constexpr std::uint32_t Error     = 0x008; // EPOLLERR / POLLERR
    inline constexpr std::uint32_t HangUp    = 0x010; // EPOLLHUP / POLLHUP
}

/// Abstract event loop interface.
///
/// Provides fd monitoring and timer scheduling. Designed as an abstraction
/// layer so the backend (epoll, io_uring) can be swapped without changing
/// any framework or user code.
///
/// Thread-safety: NOT thread-safe. Each EventLoop is owned by exactly
/// one thread (shard) in the thread-per-core model.
class EventLoop {
public:
    /// Callback for fd events. Parameter is the event mask (EPOLLIN, etc.)
    using FdCallback    = std::move_only_function<void(std::uint32_t events)>;

    /// Callback for timer expiry
    using TimerCallback = std::move_only_function<void()>;

    virtual ~EventLoop() = default;

    // ─── FD management ───────────────────────────────────────────────
    /// Register a file descriptor for monitoring.
    /// events: bitmask of EPOLLIN, EPOLLOUT, etc.
    virtual void add_fd(int fd, std::uint32_t events, FdCallback cb) = 0;

    /// Modify the event mask for an already-registered fd
    virtual void modify_fd(int fd, std::uint32_t events) = 0;

    /// Remove a file descriptor from monitoring
    virtual void remove_fd(int fd) = 0;

    // ─── Timer management ────────────────────────────────────────────
    /// Schedule a one-shot timer. Returns a handle for cancellation.
    virtual TimerHandle add_timer(Duration delay, TimerCallback cb) = 0;

    /// Schedule a timer at an absolute time point
    virtual TimerHandle add_timer_at(TimePoint when, TimerCallback cb) = 0;

    /// Cancel a previously scheduled timer (no-op if already fired)
    virtual void cancel_timer(TimerHandle handle) = 0;

    // ─── Loop control ────────────────────────────────────────────────
    /// Run the event loop (blocks until stop() is called)
    virtual void run() = 0;

    /// Run a single iteration of the event loop (non-blocking)
    virtual void run_once() = 0;

    /// Signal the event loop to stop
    virtual void stop() = 0;

    /// Check if the loop is running
    [[nodiscard]] virtual bool is_running() const noexcept = 0;

    /// Get current time (may be cached per iteration for consistency)
    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};

} // namespace novaboot::core
