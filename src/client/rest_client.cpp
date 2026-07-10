#include "novaboot/client/rest_client.h"

#include <netdb.h>
#include <sys/socket.h>

#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>

#include "novaboot/core/event_loop.h"
#include "novaboot/http3/http3_client_session.h"
#include "novaboot/net/address.h"
#include "novaboot/net/packet.h"
#include "novaboot/net/udp_socket.h"
#include "novaboot/quic/quic_client_connection.h"
#include "novaboot/router/json.h"

namespace novaboot::client {

namespace {

/// Resolve hostname to IP string using getaddrinfo
std::string resolve_host(const std::string& host, uint16_t port) {
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_ADDRCONFIG;

    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);

    int rv = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rv != 0) {
        throw ClientError(
            std::format("DNS resolution failed for '{}': {}", host, gai_strerror(rv)));
    }

    char ip_buf[INET6_ADDRSTRLEN] = {};
    if (res->ai_family == AF_INET) {
        auto* sa4 = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        inet_ntop(AF_INET, &sa4->sin_addr, ip_buf, sizeof(ip_buf));
    } else {
        auto* sa6 = reinterpret_cast<sockaddr_in6*>(res->ai_addr);
        inet_ntop(AF_INET6, &sa6->sin6_addr, ip_buf, sizeof(ip_buf));
    }
    freeaddrinfo(res);
    return std::string(ip_buf);
}

} // anonymous namespace

RestClient::RestClient() = default;

RestClient::~RestClient() {
    if (conn_) conn_->close();
    if (socket_ && event_loop_) {
        event_loop_->remove_fd(socket_->fd());
    }
}

std::unique_ptr<RestClient> RestClient::create(const Config& cfg,
                                               core::EventLoop& event_loop) {
    auto client = std::unique_ptr<RestClient>(new RestClient());
    client->cfg_        = cfg;
    client->event_loop_ = &event_loop;

    // Build TLS context
    quic::TlsContext::ClientConfig tls_cfg;
    tls_cfg.verify_peer = cfg.verify_ssl;
    tls_cfg.ca_file     = cfg.ca_file;
    client->tls_ctx_    = quic::TlsContext::create_client(tls_cfg);

    // Resolve hostname if IP not provided
    if (client->cfg_.ip.empty()) {
        client->cfg_.ip = resolve_host(cfg.host, cfg.port);
        spdlog::info("RestClient: {}:{} resolved → {}",
                     cfg.host, cfg.port, client->cfg_.ip);
    }

    // Connect
    client->do_connect();
    return client;
}

void RestClient::do_connect() {
    // Create UDP socket bound to ephemeral local port
    net::Address remote_addr = net::Address::from_string(cfg_.ip, cfg_.port);

    // Bind to the correct wildcard address based on the remote IP family
    // (IPv4 socket can't reach IPv6 destinations and vice versa)
    std::string local_bind = remote_addr.is_v6() ? "::" : "0.0.0.0";

    net::UdpSocketConfig sock_cfg;
    sock_cfg.bind_address  = net::Address::from_string(local_bind, 0);
    sock_cfg.reuse_port    = false; // client doesn't need SO_REUSEPORT
    sock_cfg.enable_gro    = false; // GRO only useful for servers
    sock_cfg.enable_gso    = false;
    sock_cfg.enable_pktinfo = true;
    sock_cfg.enable_ecn    = true;

    auto sock_result = net::UdpSocket::create(sock_cfg);
    if (!sock_result) {
        throw ClientError("RestClient: failed to create UDP socket");
    }
    socket_ = std::make_unique<net::UdpSocket>(std::move(*sock_result));

    net::Address local_addr = net::Address::from_string(local_bind,
                                                        socket_->local_port());


    // Create QUIC connection
    conn_ = quic::QuicClientConnection::create(
        cfg_.host, remote_addr, local_addr, *tls_ctx_, *event_loop_,
        [this](const net::OutgoingPacket& pkt) { send_packet(pkt); });

    // On disconnect → schedule reconnect
    conn_->set_disconnect_callback([this]() { on_disconnect(); });

    // On handshake done → create HTTP/3 session
    conn_->set_handshake_callback([this](quic::QuicClientConnection& qconn) {
        auto h3 = http3::Http3ClientSession::create(
            qconn.native_handle(),
            [this](int64_t stream_id, http3::ClientResponse resp) {
                // Response arrived — find the waiting coroutine
                auto it = pending_.find(stream_id);
                if (it != pending_.end()) {
                    it->second.first = std::move(resp);
                    auto handle = it->second.second;
                    if (handle) handle.resume(); // wake up the co_await
                }
            });
        if (!h3) {
            spdlog::error("RestClient: failed to create HTTP/3 client session");
            return;
        }
        qconn.set_http3_session(std::move(h3));
        spdlog::info("RestClient: connected to {}:{}", cfg_.host, cfg_.port);
        // Reset reconnect counters on successful connect
        reconnect_attempts_ = 0;
        next_backoff_       = std::chrono::milliseconds{100};
    });

    // Register the UDP fd on the event loop for async packet reception
    event_loop_->start_packet_recv(
        socket_->fd(),
        [this](net::IncomingPacket&& pkt) { on_packet_received(std::move(pkt)); });

    // Kick off handshake by writing the Initial packet
    conn_->on_write();

    spdlog::debug("RestClient: initiating QUIC handshake → {}:{}", cfg_.ip, cfg_.port);
}

void RestClient::on_packet_received(net::IncomingPacket&& pkt) {
    if (!conn_) return;
    conn_->on_read(pkt, quic::QuicClientConnection::timestamp_now());
    conn_->on_write();
}

void RestClient::send_packet(const net::OutgoingPacket& pkt) {
    if (!socket_) return;
    auto res = socket_->send_one(pkt);
    if (!res) {
        spdlog::error("RestClient: send_one failed with error code {}", static_cast<int>(res.error()));
    } else {
        spdlog::debug("RestClient: sent packet of size {} to {}", *res, pkt.remote.to_string());
    }
}

void RestClient::on_disconnect() {
    spdlog::warn("RestClient: connection to {}:{} lost", cfg_.host, cfg_.port);

    // Fail all pending requests with an error
    for (auto& [stream_id, entry] : pending_) {
        entry.first = http3::ClientResponse{0, {}, "Connection lost"};
        if (entry.second) entry.second.resume();
    }
    pending_.clear();

    if (reconnect_pending_) return;
    reconnect_pending_ = true;

    if (reconnect_attempts_ >= cfg_.max_reconnect_attempts) {
        spdlog::error("RestClient: max reconnect attempts ({}) reached",
                      cfg_.max_reconnect_attempts);
        return;
    }

    spdlog::info("RestClient: reconnecting in {}ms (attempt {}/{})",
                 next_backoff_.count(),
                 reconnect_attempts_ + 1,
                 cfg_.max_reconnect_attempts);

    reconnect_timer_ = event_loop_->add_timer(next_backoff_, [this]() {
        reconnect_pending_ = false;
        do_reconnect();
    });

    // Exponential backoff with 30s cap
    next_backoff_ = std::min(next_backoff_ * 2, kMaxBackoff);
    ++reconnect_attempts_;
}

void RestClient::do_reconnect() {
    spdlog::info("RestClient: attempting reconnect to {}:{}", cfg_.host, cfg_.port);
    try {
        // Remove old socket from event loop
        if (socket_) {
            event_loop_->remove_fd(socket_->fd());
        }
        conn_.reset();
        socket_.reset();
        do_connect();
    } catch (const std::exception& e) {
        spdlog::error("RestClient: reconnect failed: {}", e.what());
        on_disconnect(); // will schedule next retry
    }
}

bool RestClient::is_connected() const noexcept {
    return conn_ && conn_->is_handshake_complete() &&
           !conn_->is_closed() && !conn_->is_draining();
}

void RestClient::wait_for_connection() {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(cfg_.connect_timeout_ms);
    while (!is_connected()) {
        if (conn_ && (conn_->is_closed() || conn_->is_draining())) {
            throw ClientError(
                std::format("RestClient: connection to {}:{} failed",
                            cfg_.host, cfg_.port));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw ClientError(
                std::format("RestClient: handshake timed out after {}ms",
                            cfg_.connect_timeout_ms));
        }
        event_loop_->run_once();
    }
}

// ─── Coroutine request implementation ────────────────────────────────────────

async::Task<http3::ClientResponse> RestClient::async_request(
    std::string_view method,
    std::string_view path,
    std::string_view body,
    const http3::HeaderMap& headers) {

    // Ensure we have a live connection — wait/handshake if not ready
    if (!is_connected()) {
        try {
            wait_for_connection();
        } catch (const std::exception& e) {
            throw ClientError(
                std::format("RestClient: failed to connect to {}:{}: {}", cfg_.host, cfg_.port, e.what()));
        }
    }

    auto* h3 = conn_->http3_session();
    if (!h3) {
        throw ClientError("RestClient: HTTP/3 session not ready");
    }

    std::string authority = cfg_.host + ":" + std::to_string(cfg_.port);
    int64_t stream_id = h3->submit_request(method, path, authority, body, headers);
    if (stream_id < 0) {
        throw ClientError(
            std::format("RestClient: failed to submit {} {}", method, path));
    }

    // Flush the request to the wire
    conn_->on_write();

    // Suspend until the response callback fills pending_[stream_id]
    // We must store the coroutine handle INSIDE the pending_ map so the
    // response callback can resume it when the response arrives.
    co_await async::EventLoopSuspend{&pending_[stream_id].second};

    // We've been resumed — find the stored response
    auto it = pending_.find(stream_id);
    if (it != pending_.end() && it->second.first) {
        auto resp = std::move(*it->second.first);
        pending_.erase(it);
        co_return resp;
    }

    throw ClientError("RestClient: response lost or connection dropped");
}

// ─── Async API ────────────────────────────────────────────────────────────────

async::Task<http3::ClientResponse> RestClient::async_get(
    std::string_view path, const http3::HeaderMap& headers) {
    return async_request("GET", path, {}, headers);
}

async::Task<http3::ClientResponse> RestClient::async_post(
    std::string_view path, std::string_view body,
    const http3::HeaderMap& headers) {
    return async_request("POST", path, body, headers);
}

async::Task<http3::ClientResponse> RestClient::async_put(
    std::string_view path, std::string_view body,
    const http3::HeaderMap& headers) {
    return async_request("PUT", path, body, headers);
}

async::Task<http3::ClientResponse> RestClient::async_del(
    std::string_view path, const http3::HeaderMap& headers) {
    return async_request("DELETE", path, {}, headers);
}

async::Task<http3::ClientResponse> RestClient::async_patch(
    std::string_view path, std::string_view body,
    const http3::HeaderMap& headers) {
    return async_request("PATCH", path, body, headers);
}

// ─── Synchronous API ─────────────────────────────────────────────────────────
// Runs the event loop until the async task completes (or timeout fires).

http3::ClientResponse RestClient::sync_execute(
    async::Task<http3::ClientResponse> task) {

    // Run event loop iterations until the task is done or timeout
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(cfg_.request_timeout_ms);

    while (!task.is_ready()) {
        event_loop_->run_once();
        if (std::chrono::steady_clock::now() >= deadline) {
            throw ClientError(
                std::format("RestClient: request timed out after {}ms",
                            cfg_.request_timeout_ms));
        }
    }

    return task.await_resume(); // throws if coroutine threw
}

http3::ClientResponse RestClient::get(std::string_view path,
                                      const http3::HeaderMap& headers) {
    wait_for_connection();
    return sync_execute(async_get(path, headers));
}

http3::ClientResponse RestClient::post(std::string_view path,
                                       std::string_view body,
                                       const http3::HeaderMap& headers) {
    wait_for_connection();
    return sync_execute(async_post(path, body, headers));
}


http3::ClientResponse RestClient::put(std::string_view path,
                                      std::string_view body,
                                      const http3::HeaderMap& headers) {
    wait_for_connection();
    return sync_execute(async_put(path, body, headers));
}

http3::ClientResponse RestClient::del(std::string_view path,
                                      const http3::HeaderMap& headers) {
    wait_for_connection();
    return sync_execute(async_del(path, headers));
}

http3::ClientResponse RestClient::patch(std::string_view path,
                                        std::string_view body,
                                        const http3::HeaderMap& headers) {
    wait_for_connection();
    return sync_execute(async_patch(path, body, headers));
}

} // namespace novaboot::client
