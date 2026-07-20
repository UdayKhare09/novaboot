#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>

#include <liburing.h>

#include "novaboot/core/event_loop.h"

namespace novaboot::core {

/// io_uring-based EventLoop implementation.
///
/// Uses io_uring for all I/O operations with the following optimizations:
///   - IORING_SETUP_COOP_TASKRUN: avoids IPI interrupts for task_work
///   - IORING_SETUP_SINGLE_ISSUER: single-threaded optimization
///   - IORING_SETUP_DEFER_TASKRUN: batches task_work processing
///   - Multishot POLL: one SQE produces continuous CQEs per fd
///   - Native IORING_OP_TIMEOUT: timers inside the ring (no timerfd)
///
/// Thread-safety: NOT thread-safe. Designed for thread-per-core model.
class IoUringEventLoop final : public EventLoop {
public:
    /// SQ ring size (number of SQEs). Must be power of 2.
    static constexpr unsigned kRingSize = 512;

    /// Max CQEs to process per iteration
    static constexpr int kMaxCQEs = 256;

    IoUringEventLoop();
    ~IoUringEventLoop() override;

    // Non-copyable, non-movable
    IoUringEventLoop(const IoUringEventLoop&) = delete;
    IoUringEventLoop& operator=(const IoUringEventLoop&) = delete;

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
    void post(std::move_only_function<void()> task) override;

    void start_packet_recv(int fd, std::move_only_function<void(net::IncomingPacket&&)> cb) override;
    void async_send(int fd, const net::OutgoingPacket& pkt) override;

private:
    // ─── User data encoding ──────────────────────────────────────────
    // High 8 bits = operation type, low 56 bits = id (fd, timer_id, or context index)
    enum class OpType : std::uint8_t {
        Poll    = 1,
        Timer   = 2,
        Cancel  = 3,
        RecvMsg = 4,
        SendMsg = 5,
    };

    static std::uint64_t encode_user_data(OpType op, std::uint64_t id) noexcept;
    static std::pair<OpType, std::uint64_t> decode_user_data(std::uint64_t ud) noexcept;

    // ─── Internal operations ─────────────────────────────────────────
    /// Submit a multishot poll SQE for the given fd
    void submit_poll(int fd, std::uint32_t events);

    /// Submit a timeout SQE for the given timer
    void submit_timer(std::uint64_t timer_id, Duration delay);

    /// Cancel an in-flight poll for the given fd
    void cancel_poll(int fd);

    /// Process a single completion queue entry
    void process_cqe(struct io_uring_cqe* cqe);
    void run_posted_tasks();

    // ─── Ring state ──────────────────────────────────────────────────
    struct io_uring ring_{};
    int wakeup_fd_ = -1;
    bool ring_initialized_ = false;
    bool running_          = false;
    std::atomic_bool accepting_tasks_ = true;
    TimePoint cached_now_;

    std::mutex posted_tasks_mutex_;
    std::deque<std::move_only_function<void()>> posted_tasks_;

    // ─── FD tracking ─────────────────────────────────────────────────
    struct FdEntry {
        int            fd;
        std::uint32_t  events;
        FdCallback     callback;
        bool           poll_active = false; // multishot poll SQE is in flight
        int            registered_index = -1; // registered index in io_uring file table
    };
    std::unordered_map<int, FdEntry> fd_entries_;

    // ─── Timer tracking ──────────────────────────────────────────────
    struct TimerEntry {
        std::uint64_t  id;
        TimerCallback  callback;
        bool           cancelled = false;
        // The kernel_timespec must remain valid until the SQE is consumed
        struct __kernel_timespec ts{};
    };
    std::unordered_map<std::uint64_t, TimerEntry> timer_entries_;
    std::uint64_t next_timer_id_ = 1;

    // ─── Async Packet Contexts & Pools ───────────────────────────────
    static constexpr std::size_t kSendContextsCount = 128;

    struct RecvContext {
        int idx = -1;
        int fd = -1;
        alignas(64) std::array<std::uint8_t, 65536> buffer{};
        struct iovec iov{};
        alignas(struct cmsghdr) std::array<std::uint8_t, 512> cmsg{};
        struct sockaddr_in6 remote_addr{};
        struct msghdr msg{};
    };

    struct SendContext {
        int idx = -1;
        bool in_use = false;
        bool notif_pending = false; // Zero-copy send notification pending from kernel
        alignas(64) std::array<std::uint8_t, 65536> buffer{};
        struct iovec iov{};
        alignas(struct cmsghdr) std::array<std::uint8_t, 256> cmsg{};
        struct sockaddr_storage remote_addr{};
        struct msghdr msg{};
    };

    void submit_recvmsg(int idx);

    // ─── Fixed File Table ────────────────────────────────────────────
    static constexpr std::size_t kMaxRegisteredFiles = 64;
    std::array<int, kMaxRegisteredFiles> registered_files_{};
    int get_or_register_file(int fd);
    void unregister_file(int fd);

    // ─── Provided Buffer Ring ────────────────────────────────────────
    static constexpr std::size_t kBufRingEntries = 32;
    static constexpr std::size_t kBufSize = 65536 + 1024;
    static constexpr int kBufRingGroup = 1;

    struct io_uring_buf_ring* buf_ring_ = nullptr;
    std::vector<std::unique_ptr<std::array<std::uint8_t, kBufSize>>> recv_buffers_;

    std::array<SendContext, kSendContextsCount> send_contexts_;
    std::vector<int> free_send_indices_;

    // Per-fd state for multishot recvmsg
    struct PacketRecvEntry {
        std::move_only_function<void(net::IncomingPacket&&)> callback;
        std::uint16_t local_port = 0;  // cached once at registration
        struct msghdr msg{};
        struct sockaddr_in6 remote_addr{};
        alignas(struct cmsghdr) std::array<std::uint8_t, 512> cmsg_buf{};
    };
    std::unordered_map<int, PacketRecvEntry> packet_recv_cbs_;
};

} // namespace novaboot::core
