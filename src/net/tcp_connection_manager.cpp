#include "novaboot/net/tcp_connection_manager.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <spdlog/spdlog.h>

namespace novaboot::net {

// ─── TcpConnection ───────────────────────────────────────────────────────────

TcpConnection::TcpConnection(int fd, SSL_CTX* ssl_ctx, std::function<void(http3::Request&, http3::Response&)> handler)
    : tls_stream_(fd, ssl_ctx), handler_(std::move(handler)) {}

bool TcpConnection::keep_alive() const noexcept {
    return std::visit([](const auto& active_session) -> bool {
        using T = std::decay_t<decltype(active_session)>;
        if constexpr (std::is_same_v<T, http1::Http1Session>) {
            return active_session.keep_alive();
        } else if constexpr (std::is_same_v<T, http2::Http2Session>) {
            return active_session.keep_alive();
        }
        return true;
    }, session_);
}

std::expected<std::vector<uint8_t>, int> TcpConnection::initiate_handshake() {
    std::vector<uint8_t> out;
    auto status_opt = tls_stream_.do_handshake(out);
    if (!status_opt) {
        return std::unexpected(status_opt.error());
    }
    return out;
}

std::expected<std::vector<uint8_t>, int> TcpConnection::on_recv(std::span<const uint8_t> network_data) {
    auto tls_opt = tls_stream_.feed_network_data(network_data);
    if (!tls_opt) {
        return std::unexpected(tls_opt.error());
    }

    std::vector<uint8_t> response_data;

    // Always append any generated handshake send flights
    if (!tls_opt->handshake_send_data.empty()) {
        response_data.insert(response_data.end(), tls_opt->handshake_send_data.begin(), tls_opt->handshake_send_data.end());
    }

    // Check if ALPN is ready and session is not yet initialized
    if (tls_stream_.handshake_status() == TlsTcpStream::HandshakeStatus::Complete &&
        std::holds_alternative<std::monostate>(session_)) {
        
        std::string_view alpn = tls_stream_.negotiated_alpn();
        if (alpn == "h2") {
            session_.emplace<http2::Http2Session>(handler_);
        } else {
            // Default/fallback to HTTP/1.1
            session_.emplace<http1::Http1Session>(handler_);
        }
    }

    if (!tls_opt->decrypted_app_data.empty()) {
        auto encrypt_cb = [this](const std::vector<uint8_t>& raw) -> std::vector<uint8_t> {
            auto enc_opt = tls_stream_.encrypt_app_data(raw);
            return enc_opt ? *enc_opt : std::vector<uint8_t>{};
        };

        std::expected<std::vector<uint8_t>, int> session_res;
        std::visit([&](auto& active_session) {
            using T = std::decay_t<decltype(active_session)>;
            if constexpr (!std::is_same_v<T, std::monostate>) {
                session_res = active_session.feed_data(tls_opt->decrypted_app_data, encrypt_cb);
            }
        }, session_);

        if (session_res) {
            response_data.insert(response_data.end(), session_res->begin(), session_res->end());
        } else if (session_res.error() != 0) {
            return std::unexpected(session_res.error());
        }
    }

    // Grab any other pending TLS send data
    std::vector<uint8_t> extra = tls_stream_.get_pending_send_data();
    response_data.insert(response_data.end(), extra.begin(), extra.end());

    return response_data;
}

// ─── TcpConnectionManager::ConnectionBuffer ──────────────────────────────────

int TcpConnectionManager::ConnectionBuffer::write_pending(int fd) {
    while (!write_buffer.empty()) {
        ssize_t n = ::send(fd, write_buffer.data(), write_buffer.size(), MSG_NOSIGNAL);
        if (n > 0) {
            write_buffer.erase(write_buffer.begin(), write_buffer.begin() + n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        } else {
            return -1;
        }
    }
    return 1;
}

int TcpConnectionManager::ConnectionBuffer::send_data(int fd, std::span<const uint8_t> data) {
    if (write_buffer.empty()) {
        ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n > 0) {
            if (static_cast<size_t>(n) < data.size()) {
                write_buffer.insert(write_buffer.end(), data.begin() + n, data.end());
            }
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                write_buffer.insert(write_buffer.end(), data.begin(), data.end());
            } else {
                return -1;
            }
        } else {
            return -1;
        }
    } else {
        write_buffer.insert(write_buffer.end(), data.begin(), data.end());
    }
    return 0;
}

// ─── TcpConnectionManager ────────────────────────────────────────────────────

void TcpConnectionManager::on_accept(int client_fd, core::EventLoop& loop, SSL_CTX* ssl_ctx) {
    int flags = ::fcntl(client_fd, F_GETFL, 0);
    ::fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    auto [it, inserted] = connections_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(client_fd),
        std::forward_as_tuple(client_fd, ssl_ctx, handler_)
    );

    if (!inserted) {
        ::close(client_fd);
        return;
    }

    auto handshake_opt = it->second.initiate_handshake();
    if (handshake_opt && !handshake_opt->empty()) {
        auto& buf = connection_buffers_[client_fd];
        if (buf.send_data(client_fd, *handshake_opt) < 0) {
            close_connection(client_fd, loop);
            return;
        }
    }

    uint32_t events = core::EventFlags::Readable;
    if (connection_buffers_[client_fd].has_pending_writes()) {
        events |= core::EventFlags::Writable;
    }

    loop.add_fd(client_fd, events, [this, client_fd, &loop](uint32_t ev) {
        if (ev & core::EventFlags::Readable) {
            spdlog::debug("TCP client fd {} became readable", client_fd);
            uint8_t read_buf[4096];
            ssize_t n = ::recv(client_fd, read_buf, sizeof(read_buf), 0);
            if (n > 0) {
                spdlog::debug("TCP client fd {} received {} raw bytes", client_fd, n);
                on_recv(client_fd, std::span<const uint8_t>(read_buf, static_cast<size_t>(n)), loop);
            } else if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    close_connection(client_fd, loop);
                }
            } else {
                close_connection(client_fd, loop); // EOF
            }
        } else if (ev & core::EventFlags::Writable) {
            auto conn_it = connections_.find(client_fd);
            if (conn_it != connections_.end()) {
                auto& buf = connection_buffers_[client_fd];
                int status = buf.write_pending(client_fd);
                if (status < 0) {
                    close_connection(client_fd, loop);
                } else if (status > 0) {
                    if (!conn_it->second.keep_alive()) {
                        close_connection(client_fd, loop);
                    } else {
                        loop.modify_fd(client_fd, core::EventFlags::Readable);
                    }
                }
            }
        }
    });
}

void TcpConnectionManager::on_recv(int client_fd, std::span<const uint8_t> data, core::EventLoop& loop) {
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;

    spdlog::debug("TCP client fd {} processing on_recv", client_fd);
    auto res_opt = it->second.on_recv(data);
    if (!res_opt) {
        spdlog::warn("TCP client fd {} on_recv returned error", client_fd);
        close_connection(client_fd, loop);
        return;
    }

    spdlog::debug("TCP client fd {} on_recv produced {} bytes of response data", client_fd, res_opt->size());

    if (!res_opt->empty()) {
        auto& buf = connection_buffers_[client_fd];
        if (buf.send_data(client_fd, *res_opt) < 0) {
            close_connection(client_fd, loop);
            return;
        }
    }

    auto& buf = connection_buffers_[client_fd];
    if (buf.has_pending_writes()) {
        loop.modify_fd(client_fd, core::EventFlags::Readable | core::EventFlags::Writable);
    }

    if (!it->second.keep_alive() && !buf.has_pending_writes()) {
        close_connection(client_fd, loop);
    }
}

void TcpConnectionManager::close_connection(int client_fd, core::EventLoop& loop) {
    loop.remove_fd(client_fd);
    connections_.erase(client_fd);
    connection_buffers_.erase(client_fd);
    ::close(client_fd);
}

} // namespace novaboot::net
