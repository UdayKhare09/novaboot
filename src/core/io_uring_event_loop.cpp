#include "novaboot/core/io_uring_event_loop.h"
#include "novaboot/net/packet.h"
#include "novaboot/net/udp_socket.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <netinet/udp.h>

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

    // Initialize recv contexts
    for (std::size_t i = 0; i < kRecvContextsCount; ++i) {
        recv_contexts_[i].idx = static_cast<int>(i);
        recv_contexts_[i].iov.iov_base = recv_contexts_[i].buffer.data();
        recv_contexts_[i].iov.iov_len  = recv_contexts_[i].buffer.size();
        recv_contexts_[i].msg.msg_name = &recv_contexts_[i].remote_addr;
        recv_contexts_[i].msg.msg_namelen = sizeof(recv_contexts_[i].remote_addr);
        recv_contexts_[i].msg.msg_iov  = &recv_contexts_[i].iov;
        recv_contexts_[i].msg.msg_iovlen = 1;
        recv_contexts_[i].msg.msg_control = recv_contexts_[i].cmsg.data();
        recv_contexts_[i].msg.msg_controllen = recv_contexts_[i].cmsg.size();
    }

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

    case OpType::RecvMsg: {
        int idx = static_cast<int>(id);
        auto& ctx = recv_contexts_[idx];

        if (cqe->res < 0) {
            if (cqe->res != -ECANCELED && running_) {
                spdlog::warn("io_uring recvmsg error on fd {} idx {}: {}",
                             ctx.fd, idx, std::strerror(-cqe->res));
            }
        } else if (cqe->res > 0) {
            net::IncomingPacket pkt;
            pkt.data = ctx.buffer.data();
            pkt.size = static_cast<std::size_t>(cqe->res);
            pkt.remote = net::Address::from_sockaddr(
                reinterpret_cast<struct sockaddr*>(&ctx.remote_addr),
                ctx.msg.msg_namelen);

            // Fetch local port
            struct sockaddr_storage local_ss{};
            socklen_t local_len = sizeof(local_ss);
            std::uint16_t local_port = 0;
            if (::getsockname(ctx.fd, reinterpret_cast<struct sockaddr*>(&local_ss),
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
            for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&ctx.msg);
                 cmsg != nullptr;
                 cmsg = CMSG_NXTHDR(&ctx.msg, cmsg)) {

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

            auto it = packet_recv_cbs_.find(ctx.fd);
            if (it != packet_recv_cbs_.end() && it->second) {
                it->second(std::move(pkt));
            }
        }

        // Re-submit the SQE
        if (running_) {
            submit_recvmsg(idx);
        }
        break;
    }

    case OpType::SendMsg: {
        int idx = static_cast<int>(id);
        auto& ctx = send_contexts_[idx];
        ctx.in_use = false;
        free_send_indices_.push_back(idx);

        if (cqe->res < 0) {
            spdlog::warn("io_uring sendmsg failed idx {}: {}", idx, std::strerror(-cqe->res));
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

    for (std::size_t i = 0; i < kRecvContextsCount; ++i) {
        recv_contexts_[i].fd = fd;
        submit_recvmsg(static_cast<int>(i));
    }
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
        spdlog::error("IoUringEventLoop: SQ full, cannot submit sendmsg!");
        ctx.in_use = false;
        free_send_indices_.push_back(idx);
        return;
    }

    io_uring_prep_sendmsg(sqe, fd, &ctx.msg, 0);
    io_uring_sqe_set_data64(sqe, encode_user_data(OpType::SendMsg, idx));

    io_uring_submit(&ring_);
}

void IoUringEventLoop::submit_recvmsg(int idx) {
    auto& ctx = recv_contexts_[idx];
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        spdlog::error("IoUringEventLoop: SQ full, cannot submit recvmsg for context {}", idx);
        return;
    }

    io_uring_prep_recvmsg(sqe, ctx.fd, &ctx.msg, 0);
    io_uring_sqe_set_data64(sqe, encode_user_data(OpType::RecvMsg, idx));
}

} // namespace novaboot::core
