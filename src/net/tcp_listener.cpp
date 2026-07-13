#include "novaboot/net/tcp_listener.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <spdlog/spdlog.h>

namespace novaboot::net {

TcpListener::~TcpListener() {
    if (fd_ != -1) {
        ::close(fd_);
    }
}

TcpListener::TcpListener(TcpListener&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        if (fd_ != -1) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

std::expected<TcpListener, TcpListener::Error> TcpListener::create(const Address& bind_addr, bool reuse_port) {
    int family = bind_addr.family();
    int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        spdlog::error("TcpListener socket() failed: {}", std::strerror(errno));
        return std::unexpected(Error::SocketCreate);
    }

    // IPv6-only or dual-stack
    if (bind_addr.is_v6()) {
        int v6only = 0; // Dual-stack
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
            spdlog::warn("TcpListener IPV6_V6ONLY failed: {}", std::strerror(errno));
        }
    }

    int optval = 1;
    // SO_REUSEADDR
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        spdlog::warn("TcpListener SO_REUSEADDR failed: {}", std::strerror(errno));
    }

    // SO_REUSEPORT
    if (reuse_port) {
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0) {
            spdlog::error("TcpListener SO_REUSEPORT failed: {}", std::strerror(errno));
            ::close(fd);
            return std::unexpected(Error::SetOption);
        }
    }

    if (::bind(fd, bind_addr.sockaddr_ptr(), bind_addr.sockaddr_len()) < 0) {
        spdlog::error("TcpListener bind() failed: {}", std::strerror(errno));
        ::close(fd);
        return std::unexpected(Error::Bind);
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        spdlog::error("TcpListener listen() failed: {}", std::strerror(errno));
        ::close(fd);
        return std::unexpected(Error::Listen);
    }

    spdlog::info("TCP listener bound and listening on {} (fd={})", bind_addr.to_string(), fd);
    return TcpListener(fd);
}

} // namespace novaboot::net
