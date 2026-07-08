#include "novaboot/core/epoll_event_loop.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <sys/epoll.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "novaboot/net/packet.h"
#include "novaboot/net/udp_socket.h"

namespace novaboot::core {

EpollEventLoop::EpollEventLoop() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::system_error(
            errno, std::system_category(), "epoll_create1 failed");
    }
    cached_now_ = Clock::now();
    recv_buf_.resize(net::kMaxPacketSize);
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

#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

void EpollEventLoop::start_packet_recv(
    int fd, std::move_only_function<void(net::IncomingPacket&&)> cb) {
    packet_recv_cbs_[fd] = std::move(cb);

    add_fd(fd, EventFlags::Readable, [this, fd](std::uint32_t /*events*/) {
        auto& callback = packet_recv_cbs_[fd];
        if (!callback) return;

        for (;;) {
            net::IncomingPacket pkt;
            struct iovec iov{};
            iov.iov_base = recv_buf_.data();
            iov.iov_len  = recv_buf_.size();

            alignas(struct cmsghdr) std::uint8_t cmsg_buf[512];
            struct msghdr msg{};
            msg.msg_name       = pkt.remote.sockaddr_ptr_mut();
            msg.msg_namelen    = sizeof(struct sockaddr_in6);
            msg.msg_iov        = &iov;
            msg.msg_iovlen     = 1;
            msg.msg_control    = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);

            ssize_t nread = ::recvmsg(fd, &msg, 0);
            if (nread < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                spdlog::warn("EpollEventLoop: recvmsg failed on fd {}: {}",
                             fd, std::strerror(errno));
                break;
            }

            pkt.data = recv_buf_.data();
            pkt.size = static_cast<std::size_t>(nread);

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

            // Parse ancillary data
            for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
                 cmsg != nullptr;
                 cmsg = CMSG_NXTHDR(&msg, cmsg)) {

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

            callback(std::move(pkt));
        }
    });
}

void EpollEventLoop::async_send(int fd, const net::OutgoingPacket& pkt) {
    struct iovec iov{};
    iov.iov_base = const_cast<std::uint8_t*>(pkt.data);
    iov.iov_len  = pkt.size;

    alignas(struct cmsghdr) std::uint8_t cmsg_buf[256];
    struct msghdr msg{};
    msg.msg_name    = const_cast<struct sockaddr*>(pkt.remote.sockaddr_ptr());
    msg.msg_namelen = pkt.remote.sockaddr_len();
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    struct cmsghdr* cmsg = nullptr;
    std::size_t cmsg_len = 0;

    if (pkt.local.family() == AF_INET || pkt.local.family() == AF_INET6) {
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        cmsg = CMSG_FIRSTHDR(&msg);

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
        if (!msg.msg_control) {
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);
            cmsg = CMSG_FIRSTHDR(&msg);
        } else {
            cmsg = CMSG_NXTHDR(&msg, cmsg);
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
        if (!msg.msg_control) {
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);
            cmsg = CMSG_FIRSTHDR(&msg);
        } else {
            cmsg = CMSG_NXTHDR(&msg, cmsg);
        }
        cmsg->cmsg_level = SOL_UDP;
        cmsg->cmsg_type  = UDP_SEGMENT;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(std::uint16_t));
        *reinterpret_cast<std::uint16_t*>(CMSG_DATA(cmsg)) = pkt.gso_segment_size;
        cmsg_len += CMSG_SPACE(sizeof(std::uint16_t));
    }

    if (msg.msg_control) {
        msg.msg_controllen = static_cast<socklen_t>(cmsg_len);
    }

    ssize_t nsent = ::sendmsg(fd, &msg, 0);
    if (nsent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            spdlog::warn("EpollEventLoop: sendmsg failed on fd {}: {}",
                         fd, std::strerror(errno));
        }
    }
}

} // namespace novaboot::core
