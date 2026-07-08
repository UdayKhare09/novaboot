#include "novaboot/core/io_uring_event_loop.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <spdlog/spdlog.h>

namespace novaboot::core {

// ─── User data encoding ──────────────────────────────────────────────────────

std::uint64_t IoUringEventLoop::encode_user_data(
    OpType op, std::uint64_t id) noexcept {
    return (static_cast<std::uint64_t>(op) << 56) | (id & 0x00FFFFFFFFFFFFFF);
}

std::pair<IoUringEventLoop::OpType, std::uint64_t>
IoUringEventLoop::decode_user_data(std::uint64_t ud) noexcept {
    auto op = static_cast<OpType>(ud >> 56);
    auto id = ud & 0x00FFFFFFFFFFFFFF;
    return {op, id};
}

// ─── Construction / Destruction ──────────────────────────────────────────────

IoUringEventLoop::IoUringEventLoop() {
    struct io_uring_params params{};
    params.flags = IORING_SETUP_COOP_TASKRUN
                 | IORING_SETUP_SINGLE_ISSUER
                 | IORING_SETUP_DEFER_TASKRUN;

    int rv = io_uring_queue_init_params(kRingSize, &ring_, &params);
    if (rv < 0) {
        // Fall back without DEFER_TASKRUN (requires SINGLE_ISSUER +
        // COOP_TASKRUN on some older kernels)
        spdlog::debug("io_uring init with DEFER_TASKRUN failed ({}), "
                      "retrying without it", std::strerror(-rv));

        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_COOP_TASKRUN
                     | IORING_SETUP_SINGLE_ISSUER;

        rv = io_uring_queue_init_params(kRingSize, &ring_, &params);
        if (rv < 0) {
            // Last resort: plain init
            spdlog::debug("io_uring init with SINGLE_ISSUER failed ({}), "
                          "using basic init", std::strerror(-rv));
            rv = io_uring_queue_init(kRingSize, &ring_, 0);
            if (rv < 0) {
                throw std::system_error(
                    -rv, std::system_category(),
                    "io_uring_queue_init failed");
            }
        }
    }

    ring_initialized_ = true;
    cached_now_ = Clock::now();

    spdlog::debug("io_uring initialized (sq_entries={}, cq_entries={}, "
                  "features=0x{:08x})",
                  params.sq_entries, params.cq_entries, params.features);
}

IoUringEventLoop::~IoUringEventLoop() {
    if (ring_initialized_) {
        io_uring_queue_exit(&ring_);
    }
}

// ─── FD Management ───────────────────────────────────────────────────────────

void IoUringEventLoop::add_fd(int fd, std::uint32_t events, FdCallback cb) {
    fd_entries_[fd] = FdEntry{fd, events, std::move(cb), false};
    submit_poll(fd, events);
}

void IoUringEventLoop::modify_fd(int fd, std::uint32_t events) {
    auto it = fd_entries_.find(fd);
    if (it == fd_entries_.end()) {
        throw std::runtime_error("modify_fd: fd not registered");
    }

    it->second.events = events;

    // Cancel the current multishot poll and resubmit with new events
    if (it->second.poll_active) {
        cancel_poll(fd);
    }
    submit_poll(fd, events);
}

void IoUringEventLoop::remove_fd(int fd) {
    auto it = fd_entries_.find(fd);
    if (it == fd_entries_.end()) {
        return;
    }

    if (it->second.poll_active) {
        cancel_poll(fd);
    }

    fd_entries_.erase(it);
}

// ─── Timer Management ────────────────────────────────────────────────────────

TimerHandle IoUringEventLoop::add_timer(Duration delay, TimerCallback cb) {
    auto id = next_timer_id_++;

    auto& entry = timer_entries_[id];
    entry.id       = id;
    entry.callback = std::move(cb);
    entry.cancelled = false;

    submit_timer(id, delay);

    return TimerHandle{id};
}

TimerHandle IoUringEventLoop::add_timer_at(TimePoint when, TimerCallback cb) {
    auto delay = when - Clock::now();
    if (delay.count() < 0) {
        delay = Duration::zero();
    }
    return add_timer(delay, std::move(cb));
}

void IoUringEventLoop::cancel_timer(TimerHandle handle) {
    if (!handle) return;

    auto it = timer_entries_.find(handle.id);
    if (it == timer_entries_.end()) return;

    it->second.cancelled = true;

    // Submit a timeout_remove to cancel the in-flight SQE
    auto* sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
        io_uring_prep_timeout_remove(
            sqe, encode_user_data(OpType::Timer, handle.id), 0);
        io_uring_sqe_set_data64(
            sqe, encode_user_data(OpType::Cancel, handle.id));
    }
}

// ─── Loop Control ────────────────────────────────────────────────────────────

void IoUringEventLoop::run() {
    running_ = true;
    while (running_) {
        run_once();
    }
}

void IoUringEventLoop::run_once() {
    // Submit all pending SQEs and wait for at least 1 CQE
    int rv = io_uring_submit_and_wait(&ring_, 1);
    if (rv < 0) {
        if (rv == -EINTR) {
            return; // Interrupted by signal, retry
        }
        if (rv == -ETIME) {
            return; // Timeout — expected for timer wakeups
        }
        throw std::system_error(
            -rv, std::system_category(),
            "io_uring_submit_and_wait failed");
    }

    cached_now_ = Clock::now();

    // Process all available CQEs
    unsigned head;
    struct io_uring_cqe* cqe;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe) {
        process_cqe(cqe);
        ++count;
    }

    io_uring_cq_advance(&ring_, count);
}

void IoUringEventLoop::stop() {
    running_ = false;

    // Submit a NOP to wake up the ring if it's blocked in submit_and_wait
    auto* sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0); // sentinel — ignored in process_cqe
        io_uring_submit(&ring_);
    }
}

bool IoUringEventLoop::is_running() const noexcept {
    return running_;
}

TimePoint IoUringEventLoop::now() const noexcept {
    return cached_now_;
}

// ─── Internal: SQE Submission ────────────────────────────────────────────────

void IoUringEventLoop::submit_poll(int fd, std::uint32_t events) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        spdlog::error("io_uring SQ full, cannot submit poll for fd {}", fd);
        return;
    }

    io_uring_prep_poll_multishot(sqe, fd, events);
    io_uring_sqe_set_data64(sqe,
        encode_user_data(OpType::Poll, static_cast<std::uint64_t>(fd)));

    auto it = fd_entries_.find(fd);
    if (it != fd_entries_.end()) {
        it->second.poll_active = true;
    }
}

void IoUringEventLoop::submit_timer(std::uint64_t timer_id, Duration delay) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        spdlog::error("io_uring SQ full, cannot submit timer {}", timer_id);
        return;
    }

    auto it = timer_entries_.find(timer_id);
    if (it == timer_entries_.end()) return;

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
    if (ns < 0) ns = 0;

    it->second.ts.tv_sec  = ns / 1'000'000'000LL;
    it->second.ts.tv_nsec = ns % 1'000'000'000LL;

    io_uring_prep_timeout(sqe, &it->second.ts, 0, 0);
    io_uring_sqe_set_data64(sqe,
        encode_user_data(OpType::Timer, timer_id));
}

void IoUringEventLoop::cancel_poll(int fd) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        spdlog::warn("io_uring SQ full, cannot cancel poll for fd {}", fd);
        return;
    }

    io_uring_prep_poll_remove(sqe,
        encode_user_data(OpType::Poll, static_cast<std::uint64_t>(fd)));
    io_uring_sqe_set_data64(sqe,
        encode_user_data(OpType::Cancel, static_cast<std::uint64_t>(fd)));

    auto it = fd_entries_.find(fd);
    if (it != fd_entries_.end()) {
        it->second.poll_active = false;
    }
}

// ─── Internal: CQE Processing ───────────────────────────────────────────────

void IoUringEventLoop::process_cqe(struct io_uring_cqe* cqe) {
    // Sentinel NOP (from stop()) — skip
    if (cqe->user_data == 0) {
        return;
    }

    auto [op, id] = decode_user_data(cqe->user_data);

    switch (op) {
    case OpType::Poll: {
        int fd = static_cast<int>(id);
        auto it = fd_entries_.find(fd);

        if (it != fd_entries_.end() && it->second.callback) {
            if (cqe->res < 0) {
                // Error on the poll — log and try to re-arm
                spdlog::debug("io_uring poll error on fd {}: {}",
                              fd, std::strerror(-cqe->res));
            } else {
                // cqe->res contains the poll event mask
                it->second.callback(
                    static_cast<std::uint32_t>(cqe->res));
            }
        }

        // Multishot: if IORING_CQE_F_MORE is NOT set, the kernel
        // cancelled the multishot. We must resubmit.
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            if (fd_entries_.contains(fd)) {
                fd_entries_[fd].poll_active = false;
                submit_poll(fd, fd_entries_[fd].events);
            }
        }
        break;
    }

    case OpType::Timer: {
        auto it = timer_entries_.find(id);
        if (it != timer_entries_.end()) {
            if (!it->second.cancelled && it->second.callback) {
                // Timer expired — fire callback
                it->second.callback();
            }
            timer_entries_.erase(it);
        }
        break;
    }

    case OpType::Cancel:
        // Cancellation completion (for poll_remove or timeout_remove).
        // Clean up the timer entry if it was a timer cancel.
        if (cqe->res == 0 || cqe->res == -ETIME || cqe->res == -ENOENT) {
            // Successfully cancelled or already expired — OK
        } else {
            spdlog::debug("io_uring cancel CQE for id {} result: {}",
                          id, cqe->res);
        }

        // Clean up cancelled timer entries
        {
            auto it = timer_entries_.find(id);
            if (it != timer_entries_.end() && it->second.cancelled) {
                timer_entries_.erase(it);
            }
        }
        break;
    }
}

} // namespace novaboot::core
