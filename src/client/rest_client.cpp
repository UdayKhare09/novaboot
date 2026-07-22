#include "novaboot/client/rest_client.h"

#include <netdb.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>
#include <nghttp2/nghttp2.h>

#include "novaboot/core/event_loop.h"
#include "novaboot/http3/http3_client_session.h"
#include "novaboot/net/address.h"
#include "novaboot/net/packet.h"
#include "novaboot/net/udp_socket.h"
#include "novaboot/net/tls_tcp_stream.h"
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
    if (event_loop_ && reconnect_timer_) {
        event_loop_->cancel_timer(reconnect_timer_);
    }
    if (conn_) conn_->close();
    if (socket_ && event_loop_) {
        event_loop_->remove_fd(socket_->fd());
    }
    if (tcp_fd_ != -1 && event_loop_) {
        event_loop_->remove_fd(tcp_fd_);
        ::close(tcp_fd_);
    }
    if (h2_session_) {
        ::nghttp2_session_del(h2_session_);
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
    if (cfg.protocol == Protocol::HTTP1_1) {
        tls_cfg.alpn = "http/1.1";
    } else if (cfg.protocol == Protocol::HTTP2) {
        tls_cfg.alpn = "h2";
    } else {
        tls_cfg.alpn = "h3";
    }
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
    if (cfg_.protocol == Protocol::HTTP1_1 || cfg_.protocol == Protocol::HTTP2) {
        do_connect_tcp();
        return;
    }

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

void RestClient::do_connect_tcp() {
    net::Address remote_addr = net::Address::from_string(cfg_.ip, cfg_.port);
    int fd = ::socket(remote_addr.is_v6() ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw ClientError("RestClient: failed to create TCP socket");
    }

    int rv = ::connect(fd, remote_addr.sockaddr_ptr(), remote_addr.sockaddr_len());
    tcp_fd_ = fd;
    tcp_connected_ = false;
    tls_handshake_done_ = false;

    // Create TlsTcpStream in client mode, passing host for SNI hostname verification
    tls_stream_ = std::make_unique<net::TlsTcpStream>(fd, tls_ctx_->native_handle(), false, cfg_.host);

    if (rv == 0) {
        tcp_connected_ = true;
        std::vector<uint8_t> out;
        auto hs = tls_stream_->do_handshake(out);
        if (!out.empty()) {
            tcp_write_buf_.insert(tcp_write_buf_.end(), out.begin(), out.end());
            write_pending_tcp_data();
        }
        if (hs && *hs == net::TlsTcpStream::HandshakeStatus::Complete) {
            tls_handshake_done_ = true;
            if (cfg_.protocol == Protocol::HTTP2) {
                init_h2_session();
            }
        }
    } else {
        if (errno != EINPROGRESS) {
            ::close(fd);
            tcp_fd_ = -1;
            throw ClientError(std::format("RestClient: connect failed: {}", std::strerror(errno)));
        }
    }

    // Register on the event loop
    uint32_t flags = core::EventFlags::Readable;
    if (!tcp_connected_ || !tcp_write_buf_.empty() ||
        tls_stream_->handshake_status() == net::TlsTcpStream::HandshakeStatus::Handshaking) {
        flags |= core::EventFlags::Writable;
    }

    tcp_event_flags_ = flags;
    event_loop_->add_fd(tcp_fd_, flags, [this](uint32_t events) {
        if (events & core::EventFlags::HangUp) {
            on_disconnect();
            return;
        }
        if (events & core::EventFlags::Writable) {
            handle_tcp_writable();
        }
        if (events & core::EventFlags::Readable) {
            handle_tcp_readable();
        }
    });
}

void RestClient::handle_tcp_writable() {
    if (!tcp_connected_) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(tcp_fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            spdlog::error("RestClient: TCP connection failed: {}", std::strerror(err));
            on_disconnect();
            return;
        }
        tcp_connected_ = true;
        spdlog::info("RestClient: TCP connected to {}:{}", cfg_.host, cfg_.port);

        std::vector<uint8_t> out;
        auto hs = tls_stream_->do_handshake(out);
        if (!out.empty()) {
            tcp_write_buf_.insert(tcp_write_buf_.end(), out.begin(), out.end());
        }
        if (hs && *hs == net::TlsTcpStream::HandshakeStatus::Complete) {
            tls_handshake_done_ = true;
            if (cfg_.protocol == Protocol::HTTP2) {
                init_h2_session();
            }
        }
    }

    write_pending_tcp_data();

    uint32_t flags = core::EventFlags::Readable;
    if (!tcp_write_buf_.empty() || !tls_handshake_done_) {
        flags |= core::EventFlags::Writable;
    }
    if (tcp_fd_ != -1 && flags != tcp_event_flags_) {
        tcp_event_flags_ = flags;
        event_loop_->modify_fd(tcp_fd_, flags);
    }
}

void RestClient::handle_tcp_readable() {
    uint8_t buf[4096];
    ssize_t n = ::recv(tcp_fd_, buf, sizeof(buf), 0);
    if (n > 0) {
        auto feed_opt = tls_stream_->feed_network_data(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        if (!feed_opt) {
            spdlog::error("RestClient: TLS feed failed");
            on_disconnect();
            return;
        }

        if (!feed_opt->handshake_send_data.empty()) {
            tcp_write_buf_.insert(tcp_write_buf_.end(), feed_opt->handshake_send_data.begin(), feed_opt->handshake_send_data.end());
            write_pending_tcp_data();
        }

        if (tls_stream_->handshake_status() == net::TlsTcpStream::HandshakeStatus::Complete) {
            if (!tls_handshake_done_) {
                tls_handshake_done_ = true;
                spdlog::info("RestClient: TLS handshake complete");
                if (cfg_.protocol == Protocol::HTTP2) {
                    init_h2_session();
                }
            }
            if (!feed_opt->decrypted_app_data.empty()) {
                process_decrypted_tcp_data(feed_opt->decrypted_app_data);
            }
        }
    } else if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            on_disconnect();
        }
    } else {
        on_disconnect(); // EOF
    }

    uint32_t flags = core::EventFlags::Readable;
    if (!tcp_write_buf_.empty() || !tls_handshake_done_) {
        flags |= core::EventFlags::Writable;
    }
    if (tcp_fd_ != -1 && flags != tcp_event_flags_) {
        tcp_event_flags_ = flags;
        event_loop_->modify_fd(tcp_fd_, flags);
    }
}

void RestClient::write_pending_tcp_data() {
    while (!tcp_write_buf_.empty()) {
        ssize_t n = ::send(tcp_fd_, tcp_write_buf_.data(), tcp_write_buf_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            tcp_write_buf_.erase(tcp_write_buf_.begin(), tcp_write_buf_.begin() + n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            spdlog::error("RestClient: TCP send failed: {}", std::strerror(errno));
            on_disconnect();
            return;
        } else {
            on_disconnect();
            return;
        }
    }
}

void RestClient::process_decrypted_tcp_data(std::span<const uint8_t> data) {
    if (cfg_.protocol == Protocol::HTTP1_1) {
        http1_recv_buf_.append(reinterpret_cast<const char*>(data.data()), data.size());
        try_parse_http1_response();
    } else if (cfg_.protocol == Protocol::HTTP2) {
        if (h2_session_) {
            ssize_t rv = nghttp2_session_mem_recv2(h2_session_, data.data(), data.size());
            if (rv < 0) {
                spdlog::error("RestClient: H2 session recv failed: {}", nghttp2_strerror((int)rv));
                on_disconnect();
                return;
            }
            // Send any generated H2 frames
            const uint8_t* out_data = nullptr;
            while (true) {
                ssize_t send_rv = nghttp2_session_mem_send2(h2_session_, &out_data);
                if (send_rv > 0) {
                    auto enc_opt = tls_stream_->encrypt_app_data(std::span<const uint8_t>(out_data, static_cast<size_t>(send_rv)));
                    if (enc_opt) {
                        tcp_write_buf_.insert(tcp_write_buf_.end(), enc_opt->begin(), enc_opt->end());
                    }
                } else {
                    break;
                }
            }
            write_pending_tcp_data();
        }
    }
}

void RestClient::try_parse_http1_response() {
    std::string_view view = http1_recv_buf_;

    size_t status_line_end = view.find("\r\n");
    if (status_line_end == std::string_view::npos) return;

    std::string_view status_line = view.substr(0, status_line_end);
    if (!status_line.starts_with("HTTP/1.")) return;

    size_t code_start = status_line.find(' ');
    if (code_start == std::string_view::npos) return;
    code_start++;
    size_t code_end = status_line.find(' ', code_start);
    if (code_end == std::string_view::npos) code_end = status_line.size();

    int status_code = 0;
    try {
        status_code = std::stoi(std::string(status_line.substr(code_start, code_end - code_start)));
    } catch (...) {
        return;
    }

    size_t headers_start = status_line_end + 2;
    size_t body_start = view.find("\r\n\r\n", headers_start);
    if (body_start == std::string_view::npos) return;

    std::string_view headers_block = view.substr(headers_start, body_start - headers_start);
    body_start += 4; // skip \r\n\r\n

    http3::HeaderMap response_headers;
    size_t content_length = 0;
    bool is_chunked = false;

    size_t pos = 0;
    while (pos < headers_block.size()) {
        size_t line_end = headers_block.find("\r\n", pos);
        if (line_end == std::string_view::npos) line_end = headers_block.size();

        std::string_view line = headers_block.substr(pos, line_end - pos);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string name = std::string(line.substr(0, colon));
            std::string value = std::string(line.substr(colon + 1));
            // trim
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            response_headers.add(name, value);

            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            if (name_lower == "content-length") {
                content_length = std::stoull(value);
            } else if (name_lower == "transfer-encoding") {
                std::string val_lower = value;
                std::transform(val_lower.begin(), val_lower.end(), val_lower.begin(), ::tolower);
                if (val_lower == "chunked") {
                    is_chunked = true;
                }
            }
        }
        pos = line_end + 2;
    }

    std::string body;
    size_t total_parsed_len = body_start;

    if (is_chunked) {
        size_t body_pos = body_start;
        while (true) {
            size_t size_line_end = http1_recv_buf_.find("\r\n", body_pos);
            if (size_line_end == std::string::npos) return;

            std::string size_str = http1_recv_buf_.substr(body_pos, size_line_end - body_pos);
            size_t chunk_size = 0;
            try {
                chunk_size = std::stoull(size_str, nullptr, 16);
            } catch (...) {
                spdlog::error("RestClient: failed to parse chunk size");
                on_disconnect();
                return;
            }

            body_pos = size_line_end + 2;
            if (chunk_size == 0) {
                if (http1_recv_buf_.size() < body_pos + 2) return;
                total_parsed_len = body_pos + 2;
                break;
            }

            if (http1_recv_buf_.size() < body_pos + chunk_size + 2) return;

            body.append(http1_recv_buf_.substr(body_pos, chunk_size));
            body_pos += chunk_size + 2;
        }
    } else {
        if (http1_recv_buf_.size() < body_start + content_length) return;
        body = http1_recv_buf_.substr(body_start, content_length);
        total_parsed_len = body_start + content_length;
    }

    // Done!
    http3::ClientResponse resp{status_code, response_headers, std::move(body)};
    http1_recv_buf_.erase(0, total_parsed_len);

    tcp_response_ = std::move(resp);
    auto handle = pending_tcp_handle_;
    pending_tcp_handle_ = nullptr;
    if (handle) {
        handle.resume();
    }
}

void RestClient::init_h2_session() {
    if (h2_session_) {
        nghttp2_session_del(h2_session_);
        h2_session_ = nullptr;
    }

    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);

    nghttp2_session_callbacks_set_on_header_callback(callbacks, h2_on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, h2_on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, h2_on_stream_close_cb);

    nghttp2_session_client_new2(&h2_session_, callbacks, this, nullptr);
    nghttp2_session_callbacks_del(callbacks);

    // Send connection preface
    nghttp2_settings_entry iv[1] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
    };
    nghttp2_submit_settings(h2_session_, NGHTTP2_FLAG_NONE, iv, 1);

    const uint8_t* out_data = nullptr;
    while (true) {
        ssize_t send_rv = nghttp2_session_mem_send2(h2_session_, &out_data);
        if (send_rv > 0) {
            auto enc_opt = tls_stream_->encrypt_app_data(std::span<const uint8_t>(out_data, static_cast<size_t>(send_rv)));
            if (enc_opt) {
                tcp_write_buf_.insert(tcp_write_buf_.end(), enc_opt->begin(), enc_opt->end());
            }
        } else {
            break;
        }
    }
    write_pending_tcp_data();
}

int RestClient::h2_on_header_cb(nghttp2_session* /*session*/,
                               const nghttp2_frame* frame,
                               const uint8_t* name, size_t namelen,
                               const uint8_t* value, size_t valuelen,
                               uint8_t /*flags*/, void* user_data) {
    auto* self = static_cast<RestClient*>(user_data);
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        int32_t stream_id = frame->hd.stream_id;
        auto& pending = self->h2_pending_[stream_id];

        std::string_view name_sv(reinterpret_cast<const char*>(name), namelen);
        std::string_view value_sv(reinterpret_cast<const char*>(value), valuelen);

        if (name_sv == ":status") {
            pending.response.status_code = std::stoi(std::string(value_sv));
        } else {
            pending.response.headers.add(name_sv, value_sv);
        }
    }
    return 0;
}

int RestClient::h2_on_data_chunk_recv_cb(nghttp2_session* /*session*/, uint8_t /*flags*/,
                                        int32_t stream_id, const uint8_t* data,
                                        size_t len, void* user_data) {
    auto* self = static_cast<RestClient*>(user_data);
    auto it = self->h2_pending_.find(stream_id);
    if (it != self->h2_pending_.end()) {
        it->second.response.body.append(reinterpret_cast<const char*>(data), len);
    }
    return 0;
}

int RestClient::h2_on_stream_close_cb(nghttp2_session* /*session*/, int32_t stream_id,
                                     uint32_t /*error_code*/, void* user_data) {
    auto* self = static_cast<RestClient*>(user_data);
    auto it = self->h2_pending_.find(stream_id);
    if (it != self->h2_pending_.end()) {
        it->second.complete = true;
        auto handle = it->second.coroutine;
        if (handle) {
            handle.resume();
        }
    }
    return 0;
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

    complete_pending_with_error("Connection lost");

    if (tcp_fd_ != -1) {
        event_loop_->remove_fd(tcp_fd_);
        ::close(tcp_fd_);
        tcp_fd_ = -1;
    }
    tcp_connected_ = false;
    tls_handshake_done_ = false;
    tcp_event_flags_ = 0;
    tls_stream_.reset();
    http1_recv_buf_.clear();

    if (h2_session_) {
        nghttp2_session_del(h2_session_);
        h2_session_ = nullptr;
    }

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

void RestClient::complete_pending_with_error(std::string_view message) {
    // Do not resume a coroutine while iterating the owning map: resumption
    // erases its own entry. Mark every request first, then resume afterward.
    std::vector<std::coroutine_handle<>> pending_handles;
    pending_handles.reserve(pending_.size() + h2_pending_.size());
    for (auto& [stream_id, entry] : pending_) {
        static_cast<void>(stream_id);
        entry.first = http3::ClientResponse{0, {}, std::string(message)};
        if (entry.second) pending_handles.push_back(entry.second);
    }

    for (auto& [stream_id, entry] : h2_pending_) {
        static_cast<void>(stream_id);
        entry.complete = true;
        entry.response = http3::ClientResponse{0, {}, std::string(message)};
        if (entry.coroutine) pending_handles.push_back(entry.coroutine);
    }

    if (pending_tcp_handle_) {
        tcp_response_ = http3::ClientResponse{0, {}, std::string(message)};
        auto handle = pending_tcp_handle_;
        pending_tcp_handle_ = nullptr;
        pending_handles.push_back(handle);
    }

    for (const auto handle : pending_handles) {
        if (handle) handle.resume();
    }
    // A resumed coroutine removes its own H2/H3 entry. Any entry without a
    // waiter is no longer useful after a connection-level failure.
    pending_.clear();
    h2_pending_.clear();
}

void RestClient::do_reconnect() {
    spdlog::info("RestClient: attempting reconnect to {}:{}", cfg_.host, cfg_.port);
    try {
        if (cfg_.protocol == Protocol::HTTP1_1 || cfg_.protocol == Protocol::HTTP2) {
            if (tcp_fd_ != -1) {
                event_loop_->remove_fd(tcp_fd_);
                ::close(tcp_fd_);
                tcp_fd_ = -1;
            }
            tcp_connected_ = false;
            tls_handshake_done_ = false;
            tls_stream_.reset();
            http1_recv_buf_.clear();
            if (h2_session_) {
                nghttp2_session_del(h2_session_);
                h2_session_ = nullptr;
            }
            do_connect_tcp();
        } else {
            if (socket_) {
                event_loop_->remove_fd(socket_->fd());
            }
            conn_.reset();
            socket_.reset();
            do_connect();
        }
    } catch (const std::exception& e) {
        spdlog::error("RestClient: reconnect failed: {}", e.what());
        on_disconnect(); // will schedule next retry
    }
}

bool RestClient::is_connected() const noexcept {
    if (cfg_.protocol == Protocol::HTTP1_1 || cfg_.protocol == Protocol::HTTP2) {
        return tcp_fd_ != -1 && tcp_connected_ && tls_handshake_done_;
    }
    return conn_ && conn_->is_handshake_complete() &&
           !conn_->is_closed() && !conn_->is_draining();
}

void RestClient::wait_for_connection(const async::CancellationToken& cancellation) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(cfg_.connect_timeout_ms);
    while (!is_connected()) {
        if (cancellation.cancelled()) throw RequestCancelled();
        if (cfg_.protocol == Protocol::HTTP1_1 || cfg_.protocol == Protocol::HTTP2) {
            if (tcp_fd_ == -1) {
                throw ClientError(
                    std::format("RestClient: connection to {}:{} failed",
                                cfg_.host, cfg_.port));
            }
        } else {
            if (conn_ && (conn_->is_closed() || conn_->is_draining())) {
                throw ClientError(
                    std::format("RestClient: connection to {}:{} failed",
                                cfg_.host, cfg_.port));
            }
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

    if (cfg_.protocol == Protocol::HTTP1_1) {
        std::string req = std::format("{} {} HTTP/1.1\r\n", method, path);
        req += std::format("Host: {}\r\n", cfg_.host);

        bool has_content_length = false;
        for (const auto& h : headers.entries()) {
            req += std::format("{}: {}\r\n", h.name, h.value);
            std::string name_lower = h.name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            if (name_lower == "content-length") {
                has_content_length = true;
            }
        }

        if (!body.empty() && !has_content_length) {
            req += std::format("Content-Length: {}\r\n", body.size());
        }
        req += "Connection: keep-alive\r\n\r\n";
        if (!body.empty()) {
            req += body;
        }

        auto enc_opt = tls_stream_->encrypt_app_data(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(req.data()), req.size()));
        if (!enc_opt) {
            throw ClientError("RestClient: failed to encrypt HTTP/1.1 request");
        }
        tcp_write_buf_.insert(tcp_write_buf_.end(), enc_opt->begin(), enc_opt->end());
        write_pending_tcp_data();

        uint32_t flags = core::EventFlags::Readable;
        if (!tcp_write_buf_.empty()) {
            flags |= core::EventFlags::Writable;
        }
        if (tcp_fd_ != -1 && flags != tcp_event_flags_) {
            tcp_event_flags_ = flags;
            event_loop_->modify_fd(tcp_fd_, flags);
        }

        co_await async::EventLoopSuspend{&pending_tcp_handle_};

        if (tcp_response_) {
            auto resp = std::move(*tcp_response_);
            tcp_response_.reset();
            co_return resp;
        }
        throw ClientError("RestClient: request failed or connection lost");
    }

    if (cfg_.protocol == Protocol::HTTP2) {
        if (!h2_session_) {
            throw ClientError("RestClient: HTTP/2 session not ready");
        }

        std::vector<nghttp2_nv> nvs;
        auto add_nv = [&](std::string_view name, std::string_view value) {
            nghttp2_nv nv;
            nv.name = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data()));
            nv.namelen = name.size();
            nv.value = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data()));
            nv.valuelen = value.size();
            nv.flags = NGHTTP2_NV_FLAG_NONE;
            nvs.push_back(nv);
        };

        add_nv(":method", method);
        add_nv(":path", path);
        add_nv(":scheme", "https");
        std::string authority = cfg_.host + ":" + std::to_string(cfg_.port);
        add_nv(":authority", authority);

        for (const auto& h : headers.entries()) {
            add_nv(h.name, h.value);
        }

        struct RequestBodySource {
            std::string_view body;
            size_t offset = 0;
        } body_source{body, 0};

        nghttp2_data_provider2 data_prd;

        auto h2_client_read_cb = [](nghttp2_session* /*session*/, int32_t /*stream_id*/,
                                    uint8_t* buf, size_t length, uint32_t* data_flags,
                                    nghttp2_data_source* source, void* /*user_data*/) -> nghttp2_ssize {
            auto* src = static_cast<RequestBodySource*>(source->ptr);
            size_t available = src->body.size() - src->offset;
            size_t to_write = std::min(length, available);
            if (to_write > 0) {
                std::memcpy(buf, src->body.data() + src->offset, to_write);
                src->offset += to_write;
            }
            if (src->offset >= src->body.size()) {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            }
            return static_cast<nghttp2_ssize>(to_write);
        };

        data_prd.source.ptr = &body_source;
        data_prd.read_callback = h2_client_read_cb;

        int32_t stream_id = nghttp2_submit_request2(
            h2_session_, nullptr, nvs.data(), nvs.size(),
            body.empty() ? nullptr : &data_prd, nullptr);

        if (stream_id < 0) {
            throw ClientError(std::format("RestClient: failed to submit H2 request: {}", nghttp2_strerror(stream_id)));
        }

        const uint8_t* out_data = nullptr;
        while (true) {
            ssize_t send_rv = nghttp2_session_mem_send2(h2_session_, &out_data);
            if (send_rv > 0) {
                auto enc_opt = tls_stream_->encrypt_app_data(std::span<const uint8_t>(out_data, static_cast<size_t>(send_rv)));
                if (enc_opt) {
                    tcp_write_buf_.insert(tcp_write_buf_.end(), enc_opt->begin(), enc_opt->end());
                }
            } else {
                break;
            }
        }
        write_pending_tcp_data();

        uint32_t flags = core::EventFlags::Readable;
        if (!tcp_write_buf_.empty()) {
            flags |= core::EventFlags::Writable;
        }
        if (tcp_fd_ != -1 && flags != tcp_event_flags_) {
            tcp_event_flags_ = flags;
            event_loop_->modify_fd(tcp_fd_, flags);
        }

        co_await async::EventLoopSuspend{&h2_pending_[stream_id].coroutine};

        auto it = h2_pending_.find(stream_id);
        if (it != h2_pending_.end() && it->second.complete) {
            auto resp = std::move(it->second.response);
            h2_pending_.erase(it);
            co_return resp;
        }

        throw ClientError("RestClient: HTTP/2 request failed or connection lost");
    }

    // QUIC (HTTP/3) connection
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

    conn_->on_write();

    co_await async::EventLoopSuspend{&pending_[stream_id].second};

    auto it = pending_.find(stream_id);
    if (it != pending_.end() && it->second.first) {
        auto resp = std::move(*it->second.first);
        pending_.erase(it);
        co_return resp;
    }

    throw ClientError("RestClient: response lost or connection dropped");
}

async::Task<http3::ClientResponse> RestClient::async_request_with_retry(
    std::string_view method,
    std::string_view path,
    std::string_view body,
    const http3::HeaderMap& headers) {
    // A caller-supplied trace context is authoritative. Otherwise establish it
    // once for the logical request so retries remain part of the same trace.
    auto request_headers = headers;
    if (cfg_.propagate_trace_context && !request_headers.has("traceparent")) {
        observability::inject_trace_context(
            observability::begin_client_span(), request_headers);
    }

    const int max_attempts = std::max(1, cfg_.max_request_attempts);
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        auto response = co_await async_request(method, path, body, request_headers);
        if (attempt == max_attempts || !cfg_.should_retry ||
            !cfg_.should_retry(method, response, attempt)) {
            co_return response;
        }
    }
    throw ClientError("RestClient retry loop terminated unexpectedly");
}

// ─── Async API ────────────────────────────────────────────────────────────────

async::Task<http3::ClientResponse> RestClient::async_get(
    std::string_view path, const http3::HeaderMap& headers) {
    return async_request_with_retry("GET", path, {}, headers);
}

async::Task<http3::ClientResponse> RestClient::async_post(
    std::string_view path, std::string_view body,
    const http3::HeaderMap& headers) {
    return async_request_with_retry("POST", path, body, headers);
}

async::Task<http3::ClientResponse> RestClient::async_put(
    std::string_view path, std::string_view body,
    const http3::HeaderMap& headers) {
    return async_request_with_retry("PUT", path, body, headers);
}

async::Task<http3::ClientResponse> RestClient::async_del(
    std::string_view path, const http3::HeaderMap& headers) {
    return async_request_with_retry("DELETE", path, {}, headers);
}

async::Task<http3::ClientResponse> RestClient::async_patch(
    std::string_view path, std::string_view body,
    const http3::HeaderMap& headers) {
    return async_request_with_retry("PATCH", path, body, headers);
}

// ─── Synchronous API ─────────────────────────────────────────────────────────

http3::ClientResponse RestClient::sync_execute(
    async::Task<http3::ClientResponse> task,
    const async::CancellationToken& cancellation) {

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(cfg_.request_timeout_ms);

    while (!task.is_ready()) {
        if (cancellation.cancelled()) {
            // Completing all pending callbacks before `task` is destroyed
            // prevents a later socket callback from resuming its coroutine.
            // RestClient owns one transport, so this also invalidates any
            // other active streams on this client instance.
            on_disconnect();
            throw RequestCancelled();
        }
        event_loop_->run_once();
        // An event-loop iteration may wait for I/O. Check again before
        // accepting a response that arrived after another thread cancelled.
        if (cancellation.cancelled()) {
            on_disconnect();
            throw RequestCancelled();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // Completing the suspended request before task destruction keeps
            // protocol callbacks from later resuming a dangling coroutine.
            on_disconnect();
            throw ClientError(
                std::format("RestClient: request timed out after {}ms",
                            cfg_.request_timeout_ms));
        }
    }

    return task.await_resume();
}

http3::ClientResponse RestClient::get(std::string_view path,
                                      const http3::HeaderMap& headers,
                                      const async::CancellationToken& cancellation) {
    wait_for_connection(cancellation);
    return sync_execute(async_get(path, headers), cancellation);
}

http3::ClientResponse RestClient::post(std::string_view path,
                                       std::string_view body,
                                       const http3::HeaderMap& headers,
                                       const async::CancellationToken& cancellation) {
    wait_for_connection(cancellation);
    return sync_execute(async_post(path, body, headers), cancellation);
}

http3::ClientResponse RestClient::put(std::string_view path,
                                      std::string_view body,
                                      const http3::HeaderMap& headers,
                                      const async::CancellationToken& cancellation) {
    wait_for_connection(cancellation);
    return sync_execute(async_put(path, body, headers), cancellation);
}

http3::ClientResponse RestClient::del(std::string_view path,
                                      const http3::HeaderMap& headers,
                                      const async::CancellationToken& cancellation) {
    wait_for_connection(cancellation);
    return sync_execute(async_del(path, headers), cancellation);
}

http3::ClientResponse RestClient::patch(std::string_view path,
                                        std::string_view body,
                                        const http3::HeaderMap& headers,
                                        const async::CancellationToken& cancellation) {
    wait_for_connection(cancellation);
    return sync_execute(async_patch(path, body, headers), cancellation);
}

} // namespace novaboot::client
