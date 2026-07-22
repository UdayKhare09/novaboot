#include "novaboot/core/shard.h"
#include "novaboot/core/error_response.h"
#include "novaboot/core/io_uring_event_loop.h"
#include "novaboot/http/static_resource.h"
#include "novaboot/http3/http3_session.h"
#include "novaboot/validation/validation.h"

#include <format>

#include <pthread.h>
#include <sched.h>

#include <spdlog/spdlog.h>

namespace novaboot::core {

Shard::Shard(const ShardConfig& config,
             const quic::TlsContext& tls_ctx,
             const router::Router& router,
             const middleware::Pipeline& pipeline)
    : config_(config),
      tls_ctx_(tls_ctx),
      router_(router),
      pipeline_(pipeline) {}

Shard::~Shard() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Shard::start() {
    thread_ = std::thread([this]() { run(); });
}

void Shard::stop() {
    if (event_loop_) {
        event_loop_->stop();
    }
}

void Shard::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Shard::is_running() const noexcept {
    return event_loop_ && event_loop_->is_running();
}

void Shard::run() {
    spdlog::debug("Shard {} starting on core {}",
                  config_.shard_id, config_.cpu_core);

    // Pin thread to CPU core
    if (config_.cpu_core >= 0) {
        pin_to_core();
    }

    // Create the event loop (unconditionally using io_uring)
    spdlog::debug("Shard {}: Using io_uring backend", config_.shard_id);
    event_loop_ = std::make_unique<IoUringEventLoop>();

    // Create and bind the UDP socket
    net::UdpSocketConfig sock_config;
    sock_config.bind_address = config_.bind_address;
    sock_config.reuse_port   = true;
    sock_config.enable_gro   = true;
    sock_config.enable_gso   = true;

    auto sock_result = net::UdpSocket::create(sock_config);
    if (!sock_result) {
        spdlog::error("Shard {}: Failed to create UDP socket",
                      config_.shard_id);
        return;
    }
    socket_ = std::make_unique<net::UdpSocket>(std::move(*sock_result));

    // Create the connection manager
    conn_mgr_ = std::make_unique<quic::ConnectionManager>(
        tls_ctx_, *event_loop_);

    // Wire up callbacks
    conn_mgr_->set_send_callback(
        [this](const net::OutgoingPacket& pkt) { send_packet(pkt); });

    conn_mgr_->set_handshake_callback(
        [this](quic::QuicConnection& conn) {
            // Create HTTP/3 session when a new connection arrives
            auto h3_session = http3::Http3Session::create(
                conn.native_handle(),
                [this](http3::Http3Stream& stream) {
                    on_request(stream);
                },
                [this, &conn](int64_t stream_id) {
                    event_loop_->post([&conn, stream_id] {
                        if (auto* session = conn.http3_session()) {
                            session->resume_sse_stream(stream_id);
                            conn.on_write();
                        }
                    });
                });
            if (!h3_session) {
                spdlog::error("Closing QUIC connection due to HTTP/3 session creation failure");
                conn.close();
            } else {
                h3_session->set_peer_address(conn.remote_addr().ip_string());
                conn.set_http3_session(std::move(h3_session));
            }
        });

    // Start async packet reception
    event_loop_->start_packet_recv(socket_->fd(),
        [this](net::IncomingPacket&& pkt) { conn_mgr_->on_packet(pkt); });

    // Start TCP Listener (same bind port)
    auto tcp_res = net::TcpListener::create(config_.bind_address, true);
    if (!tcp_res) {
        spdlog::error("Shard {}: Failed to create TCP listener", config_.shard_id);
        return;
    }
    tcp_listener_ = std::make_unique<net::TcpListener>(std::move(*tcp_res));

    // Route TCP requests through the middleware pipeline.
    // The pipeline always runs first so middleware (e.g. CORS) can act on
    // every request — including OPTIONS preflight on unregistered paths.
    auto tcp_req_handler = [this](http3::Request& req, http3::Response& res) {
        // Build the final handler that contains routing + 404 logic.
        router::Handler final_handler =
            [this](http3::Request& rq, http3::Response& rs,
                   context::RequestContext& ctx) {
                auto result = router_.match(rq.method(), rq.path());
                if (!result.handler) {
                    if (rq.method() == "GET" || rq.method() == "HEAD") {
                        if (serve_static_file(rq, rs,
                                              rq.method() == "HEAD")) {
                            return;
                        }
                    }
                    rs.status(404)
                      .header("content-type", "text/plain")
                      .body("Not Found");
                    return;
                }
                rq.path_params() = std::move(result.params);
                try {
                    (*result.handler)(rq, rs, ctx);
                } catch (const novaboot::json::BindingException& binding_error) {
                    rs.status(400).header("content-type", "application/json");
                    std::string err_json = R"({"error":"Bad Request","message":"Invalid JSON request body","errors":[)";
                    bool first = true;
                    for (const auto& err : binding_error.errors()) {
                        if (!first) err_json += ',';
                        first = false;
                        err_json += "\"" + json_escape(err) + "\"";
                    }
                    rs.body(err_json + "]}");
                } catch (const novaboot::validation::ValidationException& val_ex) {
                    rs.status(400)
                      .header("content-type", "application/json");
                    std::string err_json = R"({"error":"Bad Request","message":"Validation failed","errors":[)";
                    bool first = true;
                    for (const auto& err : val_ex.errors()) {
                        if (!first) err_json += ",";
                        first = false;
                        err_json += "\"" + json_escape(err) + "\"";
                    }
                    err_json += "]}";
                    rs.body(err_json);
                } catch (const std::exception& ex) {
                    if (!router_.handle_exception(ex, rs, ctx)) {
                        spdlog::error("Unhandled route exception: {}", ex.what());
                        write_unhandled_error(rs, ex, config_.expose_error_details);
                    }
                } catch (...) {
                    spdlog::error("Unhandled non-standard route exception");
                    write_unknown_error(rs);
                }
            };

        context::RequestContext ctx;
        pipeline_.execute(req, res, ctx, final_handler);
    };

    auto websocket_upgrade_handler = [this](http3::Request& request)
        -> http1::Http1Session::UpgradeResult {
        auto match = router_.match_websocket(request.path());
        if (!match.handler) return {};
        request.path_params() = std::move(match.params);
        auto handler = *match.handler;
        const auto subprotocol = websocket::select_subprotocol(request, handler.subprotocols);
        if (handler.require_subprotocol && !subprotocol) {
            return http1::Http1Session::UpgradeResult::reject(
                400, "Required WebSocket subprotocol was not offered");
        }
        if (handler.authorize) {
            auto decision = handler.authorize(request);
            if (!decision.accepted) {
                return http1::Http1Session::UpgradeResult::reject(
                    decision.rejection_status, std::move(decision.rejection_body));
            }
            return http1::Http1Session::UpgradeResult::accept(
                std::move(handler), std::move(decision.principal), subprotocol.value_or(""));
        }
        return http1::Http1Session::UpgradeResult::accept(
            std::move(handler), {}, subprotocol.value_or(""));
    };

    auto http2_websocket_handler = [this](http3::Request& request)
        -> http2::Http2Session::WebSocketConnectResult {
        auto match = router_.match_websocket(request.path());
        if (!match.handler) return {};
        request.path_params() = std::move(match.params);
        auto handler = *match.handler;
        const auto subprotocol = websocket::select_subprotocol(request, handler.subprotocols);
        if (handler.require_subprotocol && !subprotocol) {
            return http2::Http2Session::WebSocketConnectResult::reject(
                400, "Required WebSocket subprotocol was not offered");
        }
        if (handler.authorize) {
            auto decision = handler.authorize(request);
            if (!decision.accepted) {
                return http2::Http2Session::WebSocketConnectResult::reject(
                    decision.rejection_status, std::move(decision.rejection_body));
            }
            return http2::Http2Session::WebSocketConnectResult::accept(
                std::move(handler), std::move(decision.principal), subprotocol.value_or(""));
        }
        return http2::Http2Session::WebSocketConnectResult::accept(
            std::move(handler), {}, subprotocol.value_or(""));
    };
    tcp_conn_mgr_ = std::make_unique<net::TcpConnectionManager>(
        std::move(tcp_req_handler), std::move(websocket_upgrade_handler),
        std::move(http2_websocket_handler));

    event_loop_->add_fd(tcp_listener_->fd(), core::EventFlags::Readable, [this](uint32_t events) {
        if (events & core::EventFlags::Readable) {
            spdlog::debug("Shard {}: TCP listener fd {} became readable", config_.shard_id, tcp_listener_->fd());
            struct sockaddr_storage addr{};
            socklen_t len = sizeof(addr);
            int client_fd = ::accept4(tcp_listener_->fd(), reinterpret_cast<struct sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd >= 0) {
                spdlog::debug("Shard {}: Accepted TCP connection on fd {}", config_.shard_id, client_fd);
                tcp_conn_mgr_->on_accept(client_fd, *event_loop_, tls_ctx_.native_handle());
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    spdlog::warn("TCP accept failed: {}", std::strerror(errno));
                }
            }
        }
    });

    // Periodic cleanup of closed connections (every 5 seconds)
    cleanup_timer_ = event_loop_->add_timer(
        std::chrono::seconds(5),
        [this]() { schedule_cleanup(); });

    spdlog::debug("Shard {} ready (udp_fd={}, connections=0)",
                  config_.shard_id, socket_->fd());
    ready_ = true;

    // Run the event loop (blocks until stop() is called)
    event_loop_->run();
    ready_ = false;

    spdlog::debug("Shard {} stopped", config_.shard_id);
}

void Shard::pin_to_core() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(config_.cpu_core, &cpuset);

    int rv = pthread_setaffinity_np(
        pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rv != 0) {
        spdlog::warn("Shard {}: Failed to pin to core {} (error {})",
                     config_.shard_id, config_.cpu_core, rv);
    } else {
        spdlog::debug("Shard {}: Pinned to core {}",
                      config_.shard_id, config_.cpu_core);
    }
}



bool Shard::serve_static_file(const http3::Request& request, http3::Response& res,
                              bool head_only) {
    return http::serve_static_resource(config_.static_resources_dir, request, res, head_only);
}

void Shard::on_request(http3::Http3Stream& stream) {
    auto& req = stream.request();
    auto& res = stream.response();

    // Always execute through the middleware pipeline first so that middleware
    // (e.g. CORS) can handle every request — including OPTIONS preflight on
    // paths that have no registered handler.
    router::Handler final_handler =
        [this](http3::Request& rq, http3::Response& rs,
               context::RequestContext& ctx) {
            auto result = router_.match(rq.method(), rq.path());
            if (!result.handler) {
                // Fallback to serving static files for GET and HEAD requests
                if (rq.method() == "GET" || rq.method() == "HEAD") {
                    if (serve_static_file(rq, rs,
                                         rq.method() == "HEAD")) {
                        return;
                    }
                }
                // 404 Not Found
                rs.status(404)
                  .header("content-type", "text/plain")
                  .body("Not Found");
                return;
            }
            rq.path_params() = std::move(result.params);
            try {
                (*result.handler)(rq, rs, ctx);
            } catch (const novaboot::json::BindingException& binding_error) {
                rs.status(400).header("content-type", "application/json");
                std::string err_json = R"({"error":"Bad Request","message":"Invalid JSON request body","errors":[)";
                bool first = true;
                for (const auto& err : binding_error.errors()) {
                    if (!first) err_json += ',';
                    first = false;
                    err_json += "\"" + json_escape(err) + "\"";
                }
                rs.body(err_json + "]}");
            } catch (const novaboot::validation::ValidationException& val_ex) {
                rs.status(400)
                  .header("content-type", "application/json");
                std::string err_json = R"({"error":"Bad Request","message":"Validation failed","errors":[)";
                bool first = true;
                for (const auto& err : val_ex.errors()) {
                    if (!first) err_json += ",";
                    first = false;
                        err_json += "\"" + json_escape(err) + "\"";
                }
                err_json += "]}";
                rs.body(err_json);
                } catch (const std::exception& ex) {
                    if (!router_.handle_exception(ex, rs, ctx)) {
                        spdlog::error("Unhandled route exception: {}", ex.what());
                        write_unhandled_error(rs, ex, config_.expose_error_details);
                    }
                } catch (...) {
                    spdlog::error("Unhandled non-standard route exception");
                    write_unknown_error(rs);
                }
        };

    context::RequestContext ctx;
    pipeline_.execute(req, res, ctx, final_handler);
}

void Shard::send_packet(const net::OutgoingPacket& packet) {
    event_loop_->async_send(socket_->fd(), packet);
}

void Shard::schedule_cleanup() {
    conn_mgr_->cleanup();
    if (event_loop_ && event_loop_->is_running()) {
        cleanup_timer_ = event_loop_->add_timer(
            std::chrono::seconds(5),
            [this]() { schedule_cleanup(); });
    }
}

} // namespace novaboot::core
