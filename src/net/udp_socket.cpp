#include "novaboot/net/udp_socket.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netinet/udp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace novaboot::net {

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : fd_(other.fd_),
      local_port_(other.local_port_),
      gso_supported_(other.gso_supported_),
      gro_supported_(other.gro_supported_),
      pktinfo_enabled_(other.pktinfo_enabled_),
      ecn_enabled_(other.ecn_enabled_),
      max_batch_size_(other.max_batch_size_) {
    other.fd_ = -1;
    other.local_port_ = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_              = other.fd_;
        local_port_      = other.local_port_;
        gso_supported_   = other.gso_supported_;
        gro_supported_   = other.gro_supported_;
        pktinfo_enabled_ = other.pktinfo_enabled_;
        ecn_enabled_     = other.ecn_enabled_;
        max_batch_size_  = other.max_batch_size_;
        other.fd_        = -1;
        other.local_port_ = 0;
    }
    return *this;
}

std::expected<UdpSocket, UdpSocket::Error>
UdpSocket::create(const UdpSocketConfig& config) {
    int family = config.bind_address.family();
    int fd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        spdlog::error("socket() failed: {}", std::strerror(errno));
        return std::unexpected(Error::SocketCreate);
    }

    UdpSocket sock(fd);
    sock.max_batch_size_ = config.max_batch_size;

    try {
        sock.configure_socket(config);
    } catch (...) {
        return std::unexpected(Error::SetOption);
    }

    if (::bind(fd, config.bind_address.sockaddr_ptr(),
               config.bind_address.sockaddr_len()) < 0) {
        spdlog::error("bind() failed: {}", std::strerror(errno));
        return std::unexpected(Error::Bind);
    }

    // Retrieve the actual bound port
    struct sockaddr_storage bound_addr{};
    socklen_t bound_len = sizeof(bound_addr);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound_addr), &bound_len) == 0) {
        if (bound_addr.ss_family == AF_INET) {
            sock.local_port_ = ntohs(reinterpret_cast<struct sockaddr_in*>(&bound_addr)->sin_port);
        } else if (bound_addr.ss_family == AF_INET6) {
            sock.local_port_ = ntohs(reinterpret_cast<struct sockaddr_in6*>(&bound_addr)->sin6_port);
        }
    }

    spdlog::info("UDP socket bound to {} (fd={})",
                 config.bind_address.to_string(), fd);

    return sock;
}

void UdpSocket::configure_socket(const UdpSocketConfig& config) {
    int optval = 1;

    // SO_REUSEADDR
    if (config.reuse_addr) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                         &optval, sizeof(optval)) < 0) {
            spdlog::warn("SO_REUSEADDR failed: {}", std::strerror(errno));
        }
    }

    // SO_REUSEPORT (critical for thread-per-core)
    if (config.reuse_port) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT,
                         &optval, sizeof(optval)) < 0) {
            throw std::runtime_error("SO_REUSEPORT failed — required for "
                                     "thread-per-core model");
        }
    }

    // Receive buffer size
    if (config.recv_buffer_size > 0) {
        int buf = config.recv_buffer_size;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    }

    // Send buffer size
    if (config.send_buffer_size > 0) {
        int buf = config.send_buffer_size;
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    }

    // IPv6-only or dual-stack
    if (config.bind_address.is_v6()) {
        int v6only = 0; // Dual-stack
        ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }

    // IP_PKTINFO / IPV6_RECVPKTINFO — needed to know local address
    if (config.enable_pktinfo) {
        if (config.bind_address.is_v6()) {
            if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVPKTINFO,
                             &optval, sizeof(optval)) == 0) {
                pktinfo_enabled_ = true;
            }
        } else {
            if (::setsockopt(fd_, IPPROTO_IP, IP_PKTINFO,
                             &optval, sizeof(optval)) == 0) {
                pktinfo_enabled_ = true;
            }
        }
    }

    // ECN
    if (config.enable_ecn) {
        if (config.bind_address.is_v6()) {
            if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVTCLASS,
                             &optval, sizeof(optval)) == 0) {
                ecn_enabled_ = true;
            }
        } else {
            if (::setsockopt(fd_, IPPROTO_IP, IP_RECVTOS,
                             &optval, sizeof(optval)) == 0) {
                ecn_enabled_ = true;
            }
        }
    }

    // UDP GRO
    if (config.enable_gro) {
        if (::setsockopt(fd_, SOL_UDP, UDP_GRO,
                         &optval, sizeof(optval)) == 0) {
            gro_supported_ = true;
            spdlog::info("UDP GRO enabled");
        } else {
            spdlog::info("UDP GRO not available: {}", std::strerror(errno));
        }
    }

    // GSO probe: check if kernel supports UDP_SEGMENT
    if (config.enable_gso) {
        int segment_size = 1200;
        if (::setsockopt(fd_, SOL_UDP, UDP_SEGMENT,
                         &segment_size, sizeof(segment_size)) == 0) {
            gso_supported_ = true;
            spdlog::info("UDP GSO enabled");

            // Remove the option; we'll set it per-send via cmsg
            segment_size = 0;
            ::setsockopt(fd_, SOL_UDP, UDP_SEGMENT,
                         &segment_size, sizeof(segment_size));
        } else {
            spdlog::info("UDP GSO not available: {}", std::strerror(errno));
        }
    }
}

std::expected<IncomingPacket, UdpSocket::Error>
UdpSocket::recv_one(std::span<std::uint8_t> buffer) {
    IncomingPacket pkt;

    struct iovec iov{};
    iov.iov_base = buffer.data();
    iov.iov_len  = buffer.size();

    // Control message buffer for pktinfo/ECN/GRO
    alignas(struct cmsghdr) std::uint8_t cmsg_buf[512];

    struct msghdr msg{};
    msg.msg_name       = pkt.remote.sockaddr_ptr_mut();
    msg.msg_namelen    = sizeof(struct sockaddr_in6); // Max size
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t nread = ::recvmsg(fd_, &msg, 0);

    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::unexpected(Error::WouldBlock);
        }
        return std::unexpected(Error::SystemError);
    }

    pkt.data = buffer.data();
    pkt.size = static_cast<std::size_t>(nread);

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
            pkt.local = Address::from_sockaddr(
                reinterpret_cast<struct sockaddr*>(&local_addr),
                sizeof(local_addr));
            pkt.local.set_port(local_port_);
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 &&
                   cmsg->cmsg_type == IPV6_PKTINFO) {
            auto* info = reinterpret_cast<struct in6_pktinfo*>(
                CMSG_DATA(cmsg));
            struct sockaddr_in6 local_addr{};
            local_addr.sin6_family = AF_INET6;
            local_addr.sin6_addr   = info->ipi6_addr;
            pkt.local = Address::from_sockaddr(
                reinterpret_cast<struct sockaddr*>(&local_addr),
                sizeof(local_addr));
            pkt.local.set_port(local_port_);
        } else if (cmsg->cmsg_level == IPPROTO_IP &&
                   cmsg->cmsg_type == IP_TOS) {
            pkt.ecn = *reinterpret_cast<std::uint8_t*>(CMSG_DATA(cmsg)) & 0x3;
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 &&
                   cmsg->cmsg_type == IPV6_TCLASS) {
            pkt.ecn = *reinterpret_cast<int*>(CMSG_DATA(cmsg)) & 0x3;
        } else if (cmsg->cmsg_level == SOL_UDP &&
                   cmsg->cmsg_type == UDP_GRO) {
            pkt.gro_segment_size =
                *reinterpret_cast<std::uint16_t*>(CMSG_DATA(cmsg));
        }
    }

    // Fix remote address length from recvmsg
    *pkt.remote.sockaddr_len_ptr() = msg.msg_namelen;

    return pkt;
}

std::expected<std::size_t, UdpSocket::Error>
UdpSocket::send_one(const OutgoingPacket& packet) {
    struct iovec iov{};
    iov.iov_base = const_cast<std::uint8_t*>(packet.data);
    iov.iov_len  = packet.size;

    alignas(struct cmsghdr) std::uint8_t cmsg_buf[256];
    std::size_t cmsg_len = 0;

    struct msghdr msg{};
    msg.msg_name    = const_cast<struct sockaddr*>(packet.remote.sockaddr_ptr());
    msg.msg_namelen = packet.remote.sockaddr_len();
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = 0;

    // Build ancillary data
    struct cmsghdr* cmsg = nullptr;

    // GSO segment size
    if (packet.gso_segment_size > 0 && gso_supported_) {
        msg.msg_controllen += CMSG_SPACE(sizeof(std::uint16_t));
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_UDP;
        cmsg->cmsg_type  = UDP_SEGMENT;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(std::uint16_t));
        std::memcpy(CMSG_DATA(cmsg), &packet.gso_segment_size,
                    sizeof(std::uint16_t));
        cmsg_len += CMSG_SPACE(sizeof(std::uint16_t));
    }

    // ECN
    if (packet.ecn > 0 && ecn_enabled_) {
        msg.msg_controllen += CMSG_SPACE(sizeof(int));
        cmsg = cmsg_len == 0
            ? CMSG_FIRSTHDR(&msg)
            : CMSG_NXTHDR(&msg, cmsg);

        if (packet.remote.is_v6()) {
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type  = IPV6_TCLASS;
        } else {
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type  = IP_TOS;
        }
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        int ecn_val = static_cast<int>(packet.ecn);
        std::memcpy(CMSG_DATA(cmsg), &ecn_val, sizeof(int));
        cmsg_len += CMSG_SPACE(sizeof(int));
    }

    if (cmsg_len == 0) {
        msg.msg_control    = nullptr;
        msg.msg_controllen = 0;
    } else {
        msg.msg_controllen = cmsg_len;
    }

    ssize_t nsent = ::sendmsg(fd_, &msg, 0);

    if (nsent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::unexpected(Error::WouldBlock);
        }
        return std::unexpected(Error::SystemError);
    }

    return static_cast<std::size_t>(nsent);
}

std::expected<int, UdpSocket::Error>
UdpSocket::recv_batch(std::vector<IncomingPacket>& packets,
                      std::span<std::uint8_t> buffer) {
    const int batch = std::min(max_batch_size_,
                               static_cast<int>(buffer.size() / kMaxUdpPayload));

    // For now, use a simple loop over recv_one.
    // TODO: Switch to recvmmsg() for true batched receives.
    const std::size_t per_pkt = kMaxUdpPayload;
    int received = 0;

    for (int i = 0; i < batch; ++i) {
        auto pkt_buf = buffer.subspan(
            static_cast<std::size_t>(i) * per_pkt, per_pkt);
        auto result = recv_one(pkt_buf);

        if (!result) {
            if (result.error() == Error::WouldBlock) {
                break; // No more data available
            }
            if (received > 0) break;
            return std::unexpected(result.error());
        }

        packets.push_back(std::move(*result));
        ++received;
    }

    return received;
}

std::expected<int, UdpSocket::Error>
UdpSocket::send_batch(std::span<OutgoingPacket> packets) {
    // TODO: Switch to sendmmsg() for true batched sends.
    int sent = 0;

    for (auto& pkt : packets) {
        auto result = send_one(pkt);
        if (!result) {
            if (result.error() == Error::WouldBlock) {
                break;
            }
            if (sent > 0) break;
            return std::unexpected(result.error());
        }
        ++sent;
    }

    return sent;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace novaboot::net
