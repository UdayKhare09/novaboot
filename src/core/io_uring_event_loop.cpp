#include "novaboot/core/io_uring_event_loop.h"
#include "novaboot/net/packet.h"
#include "novaboot/net/udp_socket.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <netinet/udp.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>

#include <spdlog/spdlog.h>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

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
    params.flags = IORING_SETUP_SQPOLL; // Enable SQPOLL

    int rv = io_uring_queue_init_params(kRingSize, &ring_, &params);
    if (rv < 0) {
        spdlog::debug("io_uring init with SQPOLL failed ({}), retrying without SQPOLL", std::strerror(-rv));

        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_COOP_TASKRUN
                     | IORING_SETUP_SINGLE_ISSUER
                     | IORING_SETUP_DEFER_TASKRUN;

        rv = io_uring_queue_init_params(kRingSize, &ring_, &params);
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
    }

    ring_initialized_ = true;
    cached_now_ = Clock::now();

    // ─── Initialize Registered Files ─────────────────────────────────
    registered_files_.fill(-1);
    io_uring_register_files(&ring_, registered_files_.data(), static_cast<unsigned int>(registered_files_.size()));

    // ─── Initialize Provided Buffer Ring ──────────────────────────────
    int error = 0;
    buf_ring_ = io_uring_setup_buf_ring(&ring_, kBufRingEntries, kBufRingGroup, 0, &error);
    if (!buf_ring_) {
        io_uring_queue_exit(&ring_);
        throw std::system_error(
            error, std::system_category(), "io_uring_setup_buf_ring failed");
    }

    recv_buffers_.reserve(kBufRingEntries);
    for (std::size_t i = 0; i < kBufRingEntries; ++i) {
        recv_buffers_.push_back(std::make_unique<std::array<std::uint8_t, kBufSize>>());
        io_uring_buf_ring_add(buf_ring_, recv_buffers_[i]->data(), kBufSize, static_cast<unsigned short>(i),
                              static_cast<int>(io_uring_buf_ring_mask(kBufRingEntries)), static_cast<int>(i));
    }
    io_uring_buf_ring_advance(buf_ring_, kBufRingEntries);

    std::memset(&recv_multishot_msg_, 0, sizeof(recv_multishot_msg_));
    recv_multishot_msg_.msg_namelen = sizeof(struct sockaddr_in6);
    recv_multishot_msg_.msg_controllen = 512;

    // Initialize send contexts
    free_send_indices_.reserve(kSendContextsCount);
    for (std::size_t i = 0; i < kSendContextsCount; ++i) {
        send_contexts_[i].idx = static_cast<int>(i);
        send_contexts_[i].iov.iov_base = send_contexts_[i].buffer.data();
        send_contexts_[i].iov.iov_len  = send_contexts_[i].buffer.size();
        send_contexts_[i].msg.msg_name = &send_contexts_[i].remote_addr;
        send_contexts_[i].msg.msg_iov  = &send_contexts_[i].iov;
        send_contexts_[i].msg.msg_iovlen = 1;
        send_contexts_[i].msg.msg_control = send_contexts_[i].cmsg.data();
        send_contexts_[i].msg.msg_controllen = send_contexts_[i].cmsg.size();
        free_send_indices_.push_back(static_cast<int>(i));
    }

    wakeup_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeup_fd_ < 0) {
        io_uring_free_buf_ring(&ring_, buf_ring_, kBufRingEntries, kBufRingGroup);
        io_uring_queue_exit(&ring_);
        throw std::system_error(
            errno, std::system_category(), "eventfd creation failed");
    }

    add_fd(wakeup_fd_, EventFlags::Readable, [this](std::uint32_t) {
        std::uint64_t val;
        [[maybe_unused]] auto n = ::read(wakeup_fd_, &val, sizeof(val));
    });

    bool sqpoll_enabled = (params.flags & IORING_SETUP_SQPOLL) != 0;
    spdlog::info("io_uring initialized (sq_entries={}, cq_entries={}, "
                 "features=0x{:08x}, SQPOLL={})",
                 params.sq_entries, params.cq_entries, params.features,
                 sqpoll_enabled ? "enabled" : "disabled");
}

IoUringEventLoop::~IoUringEventLoop() {
    if (wakeup_fd_ >= 0) {
        ::close(wakeup_fd_);
    }
    if (buf_ring_) {
        io_uring_free_buf_ring(&ring_, buf_ring_, kBufRingEntries, kBufRingGroup);
    }
    if (ring_initialized_) {
        io_uring_queue_exit(&ring_);
    }
}

int IoUringEventLoop::get_or_register_file(int fd) {
    for (std::size_t i = 0; i < kMaxRegisteredFiles; ++i) {
        if (registered_files_[i] == fd) {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < kMaxRegisteredFiles; ++i) {
        if (registered_files_[i] == -1) {
            registered_files_[i] = fd;
            io_uring_register_files_update(&ring_, static_cast<unsigned int>(i), &fd, 1);
            return static_cast<int>(i);
        }
    }
    spdlog::warn("get_or_register_file: Registered file table is full for fd {}", fd);
    return -1;
}

void IoUringEventLoop::unregister_file(int fd) {
    for (std::size_t i = 0; i < kMaxRegisteredFiles; ++i) {
        if (registered_files_[i] == fd) {
            registered_files_[i] = -1;
            int sentinel = -1;
            io_uring_register_files_update(&ring_, static_cast<unsigned int>(i), &sentinel, 1);
            return;
        }
    }
}

// ─── FD Management ───────────────────────────────────────────────────────────

void IoUringEventLoop::add_fd(int fd, std::uint32_t events, FdCallback cb) {
    int idx = get_or_register_file(fd);
    fd_entries_[fd] = FdEntry{fd, events, std::move(cb), false, idx};
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

    unregister_file(fd);
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
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
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
    if (wakeup_fd_ >= 0) {
        std::uint64_t val = 1;
        [[maybe_unused]] auto n = ::write(wakeup_fd_, &val, sizeof(val));
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
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe) {
        spdlog::error("io_uring SQ full, cannot submit poll for fd {}", fd);
        return;
    }

    auto it = fd_entries_.find(fd);
    int fd_val = fd;
    if (it != fd_entries_.end() && it->second.registered_index != -1) {
        fd_val = it->second.registered_index;
    }

    io_uring_prep_poll_multishot(sqe, fd_val, events);
    if (fd_val != fd) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    io_uring_sqe_set_data64(sqe,
        encode_user_data(OpType::Poll, static_cast<std::uint64_t>(fd)));

    if (it != fd_entries_.end()) {
        it->second.poll_active = true;
    }
}

void IoUringEventLoop::submit_timer(std::uint64_t timer_id, Duration delay) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
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
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
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

    case OpType::RecvMsg: {
        int fd = static_cast<int>(id);

        if (cqe->res < 0) {
            if (cqe->res != -ECANCELED && cqe->res != -EAGAIN && cqe->res != -EWOULDBLOCK && running_) {
                spdlog::warn("io_uring recvmsg error on fd {}: {}",
                             fd, std::strerror(-cqe->res));
            }
        } else if (cqe->res > 0) {
            std::uint32_t buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            auto& buffer = *recv_buffers_[buf_id];

            auto* out = io_uring_recvmsg_validate(buffer.data(), cqe->res, &recv_multishot_msg_);
            if (!out) {
                spdlog::error("io_uring_recvmsg_validate failed!");
                io_uring_buf_ring_add(buf_ring_, buffer.data(), kBufSize, buf_id, io_uring_buf_ring_mask(kBufRingEntries), 0);
                io_uring_buf_ring_advance(buf_ring_, 1);
                break;
            }

            net::IncomingPacket pkt;
            void* name_ptr = io_uring_recvmsg_name(out);
            pkt.remote = net::Address::from_sockaddr(
                reinterpret_cast<struct sockaddr*>(name_ptr),
                out->namelen);

            // Fetch local port
            struct sockaddr_storage local_ss{};
            socklen_t local_len = sizeof(local_ss);
            std::uint16_t local_port = 0;
            if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&local_ss),
                             &local_len) == 0) {
                if (local_ss.ss_family == AF_INET) {
                    local_port = ntohs(
                        reinterpret_cast<struct sockaddr_in*>(&local_ss)->sin_port);
                } else if (local_ss.ss_family == AF_INET6) {
                    local_port = ntohs(
                        reinterpret_cast<struct sockaddr_in6*>(&local_ss)->sin6_port);
                }
            }
            pkt.local.set_port(local_port);

            // Parse ancillary data
            for (struct cmsghdr* cmsg = io_uring_recvmsg_cmsg_firsthdr(out, &recv_multishot_msg_);
                 cmsg != nullptr;
                 cmsg = io_uring_recvmsg_cmsg_nexthdr(out, &recv_multishot_msg_, cmsg)) {

                if (cmsg->cmsg_level == IPPROTO_IP &&
                    cmsg->cmsg_type == IP_PKTINFO) {
                    auto* info = reinterpret_cast<struct in_pktinfo*>(CMSG_DATA(cmsg));
                    struct sockaddr_in local_addr{};
                    local_addr.sin_family = AF_INET;
                    local_addr.sin_addr   = info->ipi_addr;
                    pkt.local = net::Address::from_sockaddr(
                        reinterpret_cast<struct sockaddr*>(&local_addr),
                        sizeof(local_addr));
                    pkt.local.set_port(local_port);
                } else if (cmsg->cmsg_level == IPPROTO_IPV6 &&
                           cmsg->cmsg_type == IPV6_PKTINFO) {
                    auto* info = reinterpret_cast<struct in6_pktinfo*>(CMSG_DATA(cmsg));
                    struct sockaddr_in6 local_addr{};
                    local_addr.sin6_family = AF_INET6;
                    local_addr.sin6_addr   = info->ipi6_addr;
                    pkt.local = net::Address::from_sockaddr(
                        reinterpret_cast<struct sockaddr*>(&local_addr),
                        sizeof(local_addr));
                    pkt.local.set_port(local_port);
                } else if (cmsg->cmsg_level == IPPROTO_IP &&
                           cmsg->cmsg_type == IP_TOS) {
                    pkt.ecn = *reinterpret_cast<std::uint8_t*>(CMSG_DATA(cmsg)) & 0x3;
                } else if (cmsg->cmsg_level == IPPROTO_IPV6 &&
                           cmsg->cmsg_type == IPV6_TCLASS) {
                    pkt.ecn = *reinterpret_cast<int*>(CMSG_DATA(cmsg)) & 0x3;
                } else if (cmsg->cmsg_level == SOL_UDP &&
                           cmsg->cmsg_type == UDP_GRO) {
                    pkt.gro_segment_size = *reinterpret_cast<std::uint16_t*>(CMSG_DATA(cmsg));
                }
            }

            void* payload = io_uring_recvmsg_payload(out, &recv_multishot_msg_);
            pkt.data = reinterpret_cast<const std::uint8_t*>(payload);
            pkt.size = out->payloadlen;

            auto it = packet_recv_cbs_.find(fd);
            if (it != packet_recv_cbs_.end() && it->second) {
                it->second(std::move(pkt));
            }

            // Recycle buffer
            io_uring_buf_ring_add(buf_ring_, buffer.data(), kBufSize, static_cast<unsigned short>(buf_id), static_cast<int>(io_uring_buf_ring_mask(kBufRingEntries)), 0);
            io_uring_buf_ring_advance(buf_ring_, 1);
        }

        // Re-submit the SQE if multishot was cancelled
        if (!(cqe->flags & IORING_CQE_F_MORE) && running_) {
            auto* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
            }
            if (sqe) {
                int fd_val = get_or_register_file(fd);
                int sqe_fd = (fd_val == -1) ? fd : fd_val;
                io_uring_prep_recvmsg_multishot(sqe, sqe_fd, &recv_multishot_msg_, 0);
                if (fd_val != -1 && fd_val != fd) {
                    sqe->flags |= IOSQE_FIXED_FILE;
                }
                sqe->flags |= IOSQE_BUFFER_SELECT;
                sqe->buf_group = kBufRingGroup;
                io_uring_sqe_set_data64(sqe, encode_user_data(OpType::RecvMsg, static_cast<std::uint64_t>(fd)));
            } else {
                spdlog::error("IoUringEventLoop: SQ full, cannot re-submit multishot recvmsg!");
            }
        }
        break;
    }

    case OpType::SendMsg: {
        int idx = static_cast<int>(id);
        auto& ctx = send_contexts_[idx];

        if (cqe->flags & IORING_CQE_F_NOTIF) {
            ctx.notif_pending = false;
        } else {
            ctx.in_use = false;
            if (cqe->res < 0) {
                spdlog::warn("io_uring sendmsg failed idx {}: {}", idx, std::strerror(-cqe->res));
            }
        }

        if (!ctx.in_use && !ctx.notif_pending) {
            free_send_indices_.push_back(idx);
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

void IoUringEventLoop::start_packet_recv(
    int fd, std::move_only_function<void(net::IncomingPacket&&)> cb) {
    packet_recv_cbs_[fd] = std::move(cb);

    // Clear O_NONBLOCK for io_uring async I/O
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    int fd_val = get_or_register_file(fd);
    int sqe_fd = (fd_val == -1) ? fd : fd_val;

    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe) {
        spdlog::error("IoUringEventLoop: SQ full, cannot submit multishot recvmsg for fd {}", fd);
        return;
    }

    io_uring_prep_recvmsg_multishot(sqe, sqe_fd, &recv_multishot_msg_, 0);
    if (fd_val != -1 && fd_val != fd) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = kBufRingGroup;

    io_uring_sqe_set_data64(sqe, encode_user_data(OpType::RecvMsg, static_cast<std::uint64_t>(fd)));

    io_uring_submit(&ring_);
}

void IoUringEventLoop::async_send(
    int fd, const net::OutgoingPacket& pkt) {
    if (free_send_indices_.empty()) {
        spdlog::warn("IoUringEventLoop: out of send contexts! Dropping packet.");
        return;
    }

    int idx = free_send_indices_.back();
    free_send_indices_.pop_back();

    auto& ctx = send_contexts_[idx];
    ctx.in_use = true;
    ctx.notif_pending = false;

    // Copy payload
    std::memcpy(ctx.buffer.data(), pkt.data, pkt.size);
    ctx.iov.iov_base = ctx.buffer.data();
    ctx.iov.iov_len  = pkt.size;

    // Set remote address
    std::memcpy(&ctx.remote_addr, pkt.remote.sockaddr_ptr(), pkt.remote.sockaddr_len());
    ctx.msg.msg_name    = &ctx.remote_addr;
    ctx.msg.msg_namelen = pkt.remote.sockaddr_len();

    // Prepare cmsghdr for local IP, ECN, and GSO
    struct cmsghdr* cmsg = nullptr;
    std::size_t cmsg_len = 0;

    if (pkt.local.family() == AF_INET || pkt.local.family() == AF_INET6) {
        ctx.msg.msg_control = ctx.cmsg.data();
        ctx.msg.msg_controllen = ctx.cmsg.size();
        cmsg = CMSG_FIRSTHDR(&ctx.msg);

        if (pkt.local.family() == AF_INET) {
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type  = IP_PKTINFO;
            cmsg->cmsg_len   = CMSG_LEN(sizeof(struct in_pktinfo));
            auto* info = reinterpret_cast<struct in_pktinfo*>(CMSG_DATA(cmsg));
            std::memset(info, 0, sizeof(*info));
            info->ipi_spec_dst = reinterpret_cast<const struct sockaddr_in*>(
                pkt.local.sockaddr_ptr())->sin_addr;
            cmsg_len += CMSG_SPACE(sizeof(struct in_pktinfo));
        } else {
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type  = IPV6_PKTINFO;
            cmsg->cmsg_len   = CMSG_LEN(sizeof(struct in6_pktinfo));
            auto* info = reinterpret_cast<struct in6_pktinfo*>(CMSG_DATA(cmsg));
            std::memset(info, 0, sizeof(*info));
            info->ipi6_addr = reinterpret_cast<const struct sockaddr_in6*>(
                pkt.local.sockaddr_ptr())->sin6_addr;
            cmsg_len += CMSG_SPACE(sizeof(struct in6_pktinfo));
        }
    }

    if (pkt.ecn > 0) {
        if (!ctx.msg.msg_control) {
            ctx.msg.msg_control = ctx.cmsg.data();
            ctx.msg.msg_controllen = ctx.cmsg.size();
            cmsg = CMSG_FIRSTHDR(&ctx.msg);
        } else {
            cmsg = CMSG_NXTHDR(&ctx.msg, cmsg);
        }
        if (pkt.remote.family() == AF_INET) {
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type  = IP_TOS;
            cmsg->cmsg_len   = CMSG_LEN(sizeof(std::uint8_t));
            *reinterpret_cast<std::uint8_t*>(CMSG_DATA(cmsg)) = static_cast<std::uint8_t>(pkt.ecn);
            cmsg_len += CMSG_SPACE(sizeof(std::uint8_t));
        } else {
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type  = IPV6_TCLASS;
            cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
            *reinterpret_cast<int*>(CMSG_DATA(cmsg)) = static_cast<int>(pkt.ecn);
            cmsg_len += CMSG_SPACE(sizeof(int));
        }
    }

    if (pkt.gso_segment_size > 0) {
        if (!ctx.msg.msg_control) {
            ctx.msg.msg_control = ctx.cmsg.data();
            ctx.msg.msg_controllen = ctx.cmsg.size();
            cmsg = CMSG_FIRSTHDR(&ctx.msg);
        } else {
            cmsg = CMSG_NXTHDR(&ctx.msg, cmsg);
        }
        cmsg->cmsg_level = SOL_UDP;
        cmsg->cmsg_type  = UDP_SEGMENT;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(std::uint16_t));
        *reinterpret_cast<std::uint16_t*>(CMSG_DATA(cmsg)) = pkt.gso_segment_size;
        cmsg_len += CMSG_SPACE(sizeof(std::uint16_t));
    }

    if (ctx.msg.msg_control) {
        ctx.msg.msg_controllen = static_cast<socklen_t>(cmsg_len);
    } else {
        ctx.msg.msg_control = nullptr;
        ctx.msg.msg_controllen = 0;
    }

    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe) {
        spdlog::error("IoUringEventLoop: SQ full, cannot submit sendmsg!");
        ctx.in_use = false;
        ctx.notif_pending = false;
        free_send_indices_.push_back(idx);
        return;
    }

    int fd_val = get_or_register_file(fd);
    int sqe_fd = (fd_val == -1) ? fd : fd_val;
    io_uring_prep_sendmsg(sqe, sqe_fd, &ctx.msg, 0);
    if (fd_val != -1 && fd_val != fd) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data64(sqe, encode_user_data(OpType::SendMsg, idx));

    io_uring_submit(&ring_);
}


} // namespace novaboot::core
