#pragma once

#include <expected>
#include <system_error>
#include "novaboot/net/address.h"

namespace novaboot::net {

class TcpListener {
public:
    enum class Error {
        SocketCreate,
        SetOption,
        Bind,
        Listen,
    };

    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    static std::expected<TcpListener, Error> create(const Address& bind_addr, bool reuse_port = true);

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    explicit TcpListener(int fd) noexcept : fd_(fd) {}
    int fd_ = -1;
};

} // namespace novaboot::net
