#pragma once

#include <expected>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <variant>
#include <openssl/ssl.h>
#include "novaboot/net/tls_tcp_stream.h"
#include "novaboot/http1/http1_session.h"
#include "novaboot/http2/http2_session.h"
#include "novaboot/core/event_loop.h"

namespace novaboot::net {

class TcpConnection {
public:
    TcpConnection(int fd, SSL_CTX* ssl_ctx, std::function<void(http3::Request&, http3::Response&)> handler);
    ~TcpConnection() = default;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&) = default;
    TcpConnection& operator=(TcpConnection&&) = default;

    [[nodiscard]] int fd() const noexcept { return tls_stream_.fd(); }
    [[nodiscard]] bool keep_alive() const noexcept;

    std::expected<std::vector<uint8_t>, int> on_recv(std::span<const uint8_t> network_data);
    std::expected<std::vector<uint8_t>, int> initiate_handshake();

private:
    TlsTcpStream tls_stream_;
    std::function<void(http3::Request&, http3::Response&)> handler_;
    std::variant<std::monostate, http1::Http1Session, http2::Http2Session> session_;
};

class TcpConnectionManager {
public:
    TcpConnectionManager(std::function<void(http3::Request&, http3::Response&)> handler)
        : handler_(std::move(handler)) {}
    ~TcpConnectionManager() = default;

    TcpConnectionManager(const TcpConnectionManager&) = delete;
    TcpConnectionManager& operator=(const TcpConnectionManager&) = delete;

    void on_accept(int client_fd, core::EventLoop& loop, SSL_CTX* ssl_ctx);
    void on_recv(int client_fd, std::span<const uint8_t> data, core::EventLoop& loop);
    void close_connection(int client_fd, core::EventLoop& loop);

private:
    std::function<void(http3::Request&, http3::Response&)> handler_;
    std::unordered_map<int, TcpConnection> connections_;
};

} // namespace novaboot::net
