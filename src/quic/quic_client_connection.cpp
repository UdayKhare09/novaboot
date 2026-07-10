#include "novaboot/quic/quic_client_connection.h"
#include "novaboot/http3/http3_client_session.h"

#include <chrono>
#include <cstring>
#include <format>
#include <random>
#include <stdexcept>

#include <openssl/rand.h>
#include <cstdarg>

#include <spdlog/spdlog.h>

namespace novaboot::quic {

namespace {

void ngtcp2_log_printf_client(void* /*user_data*/, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    spdlog::debug("[ngtcp2-client] {}", buf);
}

void generate_random_client(uint8_t* dest, size_t len) {
    RAND_bytes(dest, static_cast<int>(len));
}

ngtcp2_tstamp get_timestamp_client() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
}

} // anonymous namespace

QuicClientConnection::~QuicClientConnection() {
    if (event_loop_ && expiry_timer_) {
        event_loop_->cancel_timer(expiry_timer_);
    }
    if (conn_) {
        ngtcp2_conn_del(conn_);
    }
}

std::unique_ptr<QuicClientConnection> QuicClientConnection::create(
    const std::string&       host,
    const net::Address&      remote_addr,
    const net::Address&      local_addr,
    const TlsContext&        tls_ctx,
    core::EventLoop&         event_loop,
    ClientSendPacketCallback send_cb) {

    auto qconn = std::unique_ptr<QuicClientConnection>(new QuicClientConnection());
    qconn->local_addr_  = local_addr;
    qconn->remote_addr_ = remote_addr;
    qconn->event_loop_  = &event_loop;
    qconn->send_cb_     = std::move(send_cb);
    qconn->pkt_buf_.resize(kMaxPktBufSize);

    // Generate client SCID (must be at least 8 bytes for QUIC Initial handshake)
    qconn->scid_.datalen = 8;
    generate_random_client(qconn->scid_.data, qconn->scid_.datalen);

    // Generate initial DCID (server's CID — must be at least 8 bytes for QUIC Initial handshake)
    ngtcp2_cid dcid;
    dcid.datalen = 8;
    generate_random_client(dcid.data, dcid.datalen);

    // Setup conn_ref for ngtcp2 ↔ OpenSSL crypto callback routing
    qconn->conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref) {
        auto* self = static_cast<QuicClientConnection*>(ref->user_data);
        return self->conn_;
    };
    qconn->conn_ref_.user_data = qconn.get();

    // Create client TLS session
    qconn->tls_session_ = TlsSession::create_client(tls_ctx, host, &qconn->conn_ref_);

    // Setup ngtcp2 callbacks
    ngtcp2_callbacks callbacks{};
    callbacks.client_initial          = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data        = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.handshake_completed     = on_handshake_completed;
    callbacks.recv_stream_data        = on_recv_stream_data;
    callbacks.stream_open             = on_stream_open;
    callbacks.stream_close            = on_stream_close;
    callbacks.acked_stream_data_offset = on_acked_stream_data_offset;
    callbacks.get_new_connection_id   = on_get_new_connection_id;
    callbacks.remove_connection_id    = on_remove_connection_id;
    callbacks.extend_max_stream_data  = on_extend_max_stream_data;
    callbacks.rand                    = on_rand;
    // Required for client: handles QUIC Retry packets (address validation)
    callbacks.recv_retry              = ngtcp2_crypto_recv_retry_cb;


    // Crypto callbacks
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx  = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;

    // Settings
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = get_timestamp_client();
    settings.log_printf = ngtcp2_log_printf_client;

    // Transport parameters
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi           = 100;
    params.initial_max_streams_uni            = 3;
    params.initial_max_data                   = 1024 * 1024;      // 1MB
    params.initial_max_stream_data_bidi_local  = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni         = 256 * 1024;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;

    // Setup path
    ngtcp2_path path;
    path.local = ngtcp2_addr{
        const_cast<sockaddr*>(local_addr.sockaddr_ptr()),
        local_addr.sockaddr_len()
    };
    path.remote = ngtcp2_addr{
        const_cast<sockaddr*>(remote_addr.sockaddr_ptr()),
        remote_addr.sockaddr_len()
    };

    // Create the ngtcp2 client connection
    int rv = ngtcp2_conn_client_new(
        &qconn->conn_,
        &dcid,           // server's DCID (initial random)
        &qconn->scid_,   // our SCID
        &path,
        NGTCP2_PROTO_VER_V1,
        &callbacks,
        &settings,
        &params,
        nullptr,         // default allocator
        qconn.get());    // user_data

    if (rv != 0) {
        throw std::runtime_error(
            std::format("ngtcp2_conn_client_new failed: {}", ngtcp2_strerror(rv)));
    }

    // Set TLS native handle
    ngtcp2_conn_set_tls_native_handle(qconn->conn_,
                                      qconn->tls_session_.crypto_ctx());

    spdlog::debug("QUIC client connection created → {}", remote_addr.to_string());

    return qconn;
}

int QuicClientConnection::on_read(const net::IncomingPacket& packet,
                                  ngtcp2_tstamp timestamp) {
    ngtcp2_path path;
    path.local = ngtcp2_addr{
        const_cast<sockaddr*>(local_addr_.sockaddr_ptr()),
        local_addr_.sockaddr_len()
    };
    path.remote = ngtcp2_addr{
        const_cast<sockaddr*>(remote_addr_.sockaddr_ptr()),
        remote_addr_.sockaddr_len()
    };

    ngtcp2_pkt_info pi{};
    pi.ecn = packet.ecn;

    int rv = ngtcp2_conn_read_pkt(conn_, &path, &pi,
                                  packet.data, packet.size,
                                  timestamp);
    if (rv != 0) {
        spdlog::debug("ngtcp2_conn_read_pkt (client) error: {}", ngtcp2_strerror(rv));
        if (ngtcp2_err_is_fatal(rv)) {
            signal_disconnect();
            return rv;
        }
        on_write();
        return rv;
    }

    update_timer();
    return 0;
}

int QuicClientConnection::on_write() {
    if (is_closed() || is_draining()) return 0;

    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;

    // Write HTTP/3 stream data first
    if (http3_session_) {
        for (;;) {
            int64_t  stream_id = -1;
            int      fin       = 0;
            ngtcp2_vec datav[16];

            auto ngdatavcnt = http3_session_->get_stream_data(
                &stream_id, &fin, datav, 16);

            if (ngdatavcnt < 0) break;

            uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
            if (fin) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

            ngtcp2_ssize pdatalen = 0;
            ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
                conn_, &ps.path, &pi,
                pkt_buf_.data(), pkt_buf_.size(),
                &pdatalen, flags, stream_id,
                datav, static_cast<size_t>(ngdatavcnt),
                timestamp_now());

            if (nwrite < 0) {
                if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                    http3_session_->add_write_offset(stream_id, static_cast<size_t>(pdatalen));
                    continue;
                }
                if (nwrite == NGTCP2_ERR_STREAM_NOT_FOUND ||
                    nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
                    http3_session_->add_write_offset(stream_id, static_cast<size_t>(pdatalen));
                    continue;
                }
                spdlog::debug("ngtcp2_conn_writev_stream (client) error: {}",
                              ngtcp2_strerror(static_cast<int>(nwrite)));
                return static_cast<int>(nwrite);
            }
            if (nwrite == 0) break;

            if (stream_id >= 0 && pdatalen > 0) {
                http3_session_->add_write_offset(stream_id, static_cast<size_t>(pdatalen));
            }

            net::OutgoingPacket out;
            out.data   = pkt_buf_.data();
            out.size   = static_cast<std::size_t>(nwrite);
            out.remote = remote_addr_;
            out.local  = local_addr_;
            out.ecn    = pi.ecn;
            send_cb_(out);
        }
    }

    // Write remaining non-stream data (handshake, ACKs)
    for (;;) {
        ngtcp2_ssize nwrite = ngtcp2_conn_write_pkt(
            conn_, &ps.path, &pi,
            pkt_buf_.data(), pkt_buf_.size(),
            timestamp_now());

        if (nwrite < 0) {
            spdlog::debug("ngtcp2_conn_write_pkt (client) error: {}",
                          ngtcp2_strerror(static_cast<int>(nwrite)));
            return static_cast<int>(nwrite);
        }
        if (nwrite == 0) break;

        net::OutgoingPacket out;
        out.data   = pkt_buf_.data();
        out.size   = static_cast<std::size_t>(nwrite);
        out.remote = remote_addr_;
        out.local  = local_addr_;
        out.ecn    = pi.ecn;
        send_cb_(out);
    }

    update_timer();
    return 0;
}

int QuicClientConnection::handle_expiry(ngtcp2_tstamp timestamp) {
    int rv = ngtcp2_conn_handle_expiry(conn_, timestamp);
    if (rv != 0) {
        spdlog::debug("ngtcp2_conn_handle_expiry (client) error: {}", ngtcp2_strerror(rv));
        signal_disconnect();
        return rv;
    }
    return on_write();
}

bool QuicClientConnection::is_draining() const noexcept {
    return conn_ && ngtcp2_conn_in_draining_period(conn_);
}

bool QuicClientConnection::is_closed() const noexcept {
    return closed_ || !conn_ || ngtcp2_conn_in_closing_period(conn_);
}

void QuicClientConnection::close(uint64_t app_error_code) {
    if (closed_ || !conn_) return;

    if (!ngtcp2_conn_in_closing_period(conn_) &&
        !ngtcp2_conn_in_draining_period(conn_)) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi;

        ngtcp2_ccerr ccerr{};
        ccerr.type = NGTCP2_CCERR_TYPE_APPLICATION;
        ccerr.error_code = app_error_code;

        ngtcp2_ssize nwrite = ngtcp2_conn_write_connection_close(
            conn_, &ps.path, &pi,
            pkt_buf_.data(), pkt_buf_.size(),
            &ccerr, get_timestamp_client());

        if (nwrite > 0) {
            net::OutgoingPacket out;
            out.data   = pkt_buf_.data();
            out.size   = static_cast<std::size_t>(nwrite);
            out.remote = remote_addr_;
            out.local  = local_addr_;
            send_cb_(out);
        }
    }
    closed_ = true;
}

ngtcp2_tstamp QuicClientConnection::get_expiry() const noexcept {
    if (!conn_) return UINT64_MAX;
    return ngtcp2_conn_get_expiry(conn_);
}

ngtcp2_tstamp QuicClientConnection::timestamp_now() noexcept {
    return get_timestamp_client();
}

void QuicClientConnection::set_http3_session(
    std::unique_ptr<http3::Http3ClientSession> session) {
    http3_session_ = std::move(session);
}

void QuicClientConnection::update_timer() {
    if (!event_loop_ || !conn_) return;
    if (expiry_timer_) event_loop_->cancel_timer(expiry_timer_);

    auto expiry = ngtcp2_conn_get_expiry(conn_);
    if (expiry == UINT64_MAX) return;

    auto now_ts = timestamp_now();
    if (expiry <= now_ts) {
        expiry_timer_ = event_loop_->add_timer(
            std::chrono::milliseconds(0),
            [this]() { handle_expiry(timestamp_now()); });
        return;
    }

    auto delay = std::chrono::nanoseconds(expiry - now_ts);
    expiry_timer_ = event_loop_->add_timer(delay, [this]() {
        handle_expiry(timestamp_now());
    });
}

void QuicClientConnection::signal_disconnect() {
    if (!closed_) {
        closed_ = true;
        if (disconnect_cb_) disconnect_cb_();
    }
}

// ─── Static callback implementations ────────────────────────────────────────

int QuicClientConnection::on_handshake_completed(ngtcp2_conn* /*conn*/,
                                                  void* user_data) {
    auto* self = static_cast<QuicClientConnection*>(user_data);
    return self->handle_handshake_completed();
}

int QuicClientConnection::handle_handshake_completed() {
    spdlog::debug("QUIC client handshake completed → {}", remote_addr_.to_string());
    handshake_done_ = true;
    if (handshake_cb_) {
        try {
            handshake_cb_(*this);
        } catch (const std::exception& e) {
            spdlog::error("Error in client handshake callback: {}", e.what());
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }
    return 0;
}

int QuicClientConnection::on_recv_stream_data(
    ngtcp2_conn* /*conn*/, uint32_t flags,
    int64_t stream_id, uint64_t offset,
    const uint8_t* data, size_t datalen,
    void* user_data, void* /*stream_user_data*/) {
    auto* self = static_cast<QuicClientConnection*>(user_data);
    return self->handle_recv_stream_data(flags, stream_id, offset, data, datalen);
}

int QuicClientConnection::handle_recv_stream_data(
    uint32_t flags, int64_t stream_id, uint64_t offset,
    const uint8_t* data, size_t datalen) {

    if (in_error_state_) return NGTCP2_ERR_CALLBACK_FAILURE;
    if (!http3_session_) return 0;

    int rv = http3_session_->on_stream_data(stream_id, offset, data, datalen,
                                            flags & NGTCP2_STREAM_DATA_FLAG_FIN);
    if (rv != 0) {
        in_error_state_ = true;
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int QuicClientConnection::on_stream_open(ngtcp2_conn* /*conn*/,
                                          int64_t /*stream_id*/,
                                          void* /*user_data*/) {
    return 0;
}

int QuicClientConnection::on_stream_close(
    ngtcp2_conn* /*conn*/, uint32_t /*flags*/,
    int64_t stream_id, uint64_t app_error_code,
    void* user_data, void* /*stream_user_data*/) {
    auto* self = static_cast<QuicClientConnection*>(user_data);
    return self->handle_stream_close(stream_id, app_error_code);
}

int QuicClientConnection::handle_stream_close(int64_t stream_id,
                                               uint64_t app_error_code) {
    if (!http3_session_) return 0;
    return http3_session_->on_stream_close(stream_id, app_error_code);
}

int QuicClientConnection::on_acked_stream_data_offset(
    ngtcp2_conn* /*conn*/,
    int64_t stream_id, uint64_t offset, uint64_t datalen,
    void* user_data, void* /*stream_user_data*/) {
    auto* self = static_cast<QuicClientConnection*>(user_data);
    return self->handle_acked_stream_data(stream_id, offset, datalen);
}

int QuicClientConnection::handle_acked_stream_data(int64_t stream_id,
                                                    uint64_t /*offset*/,
                                                    uint64_t datalen) {
    if (!http3_session_) return 0;
    return http3_session_->on_acked_stream_data(stream_id, 0, datalen);
}

int QuicClientConnection::on_get_new_connection_id(
    ngtcp2_conn* /*conn*/,
    ngtcp2_cid* cid, uint8_t* token,
    size_t cidlen, void* /*user_data*/) {
    cid->datalen = cidlen;
    generate_random_client(cid->data, cidlen);
    generate_random_client(token, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

int QuicClientConnection::on_remove_connection_id(ngtcp2_conn* /*conn*/,
                                                   const ngtcp2_cid* /*cid*/,
                                                   void* /*user_data*/) {
    return 0;
}

int QuicClientConnection::on_extend_max_stream_data(
    ngtcp2_conn* /*conn*/, int64_t stream_id,
    uint64_t /*max_data*/, void* user_data, void* /*stream_user_data*/) {
    auto* self = static_cast<QuicClientConnection*>(user_data);
    return self->handle_extend_max_stream_data(stream_id, 0);
}

int QuicClientConnection::handle_extend_max_stream_data(int64_t stream_id,
                                                         uint64_t /*max_data*/) {
    if (!http3_session_) return 0;
    return http3_session_->on_extend_max_stream_data(stream_id);
}

void QuicClientConnection::on_rand(uint8_t* dest, size_t destlen,
                                    const ngtcp2_rand_ctx* /*rand_ctx*/) {
    generate_random_client(dest, destlen);
}

} // namespace novaboot::quic
