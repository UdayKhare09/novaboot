#include "novaboot/http3/http3_client_session.h"

#include <cstring>
#include <format>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace novaboot::http3 {

Http3ClientSession::~Http3ClientSession() {
    if (conn_) {
        nghttp3_conn_del(conn_);
    }
}

std::unique_ptr<Http3ClientSession> Http3ClientSession::create(
    ngtcp2_conn* quic_conn,
    ResponseHandler resp_handler) {

    auto session = std::unique_ptr<Http3ClientSession>(new Http3ClientSession());
    session->quic_conn_ = quic_conn;
    session->handler_   = std::move(resp_handler);

    // Callbacks
    nghttp3_callbacks callbacks{};
    callbacks.recv_header       = on_recv_header;
    callbacks.end_headers       = on_end_headers;
    callbacks.recv_data         = on_recv_data;
    callbacks.end_stream        = on_end_stream;
    callbacks.acked_stream_data = on_acked_stream_data_cb;
    callbacks.deferred_consume  = on_deferred_consume;

    // Settings (same as server for consistency)
    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.qpack_max_dtable_capacity        = 4096;
    settings.qpack_encoder_max_dtable_capacity = 4096;
    settings.qpack_blocked_streams             = 100;

    // Create the client-side nghttp3 connection
    int rv = nghttp3_conn_client_new(
        &session->conn_,
        &callbacks,
        &settings,
        nghttp3_mem_default(),
        session.get()); // user_data = this

    if (rv != 0) {
        spdlog::error("nghttp3_conn_client_new failed: {}", nghttp3_strerror(rv));
        return nullptr;
    }

    // Bind control and QPACK streams
    // Client unidirectional streams: 2 (control), 6 (qenc), 10 (qdec)
    int64_t ctrl_stream_id, qenc_stream_id, qdec_stream_id;

    rv = ngtcp2_conn_open_uni_stream(quic_conn, &ctrl_stream_id, nullptr);
    if (rv != 0) {
        spdlog::error("Client: failed to open control stream: {}", ngtcp2_strerror(rv));
        return nullptr;
    }
    rv = nghttp3_conn_bind_control_stream(session->conn_, ctrl_stream_id);
    if (rv != 0) {
        spdlog::error("Client: failed to bind control stream: {}", nghttp3_strerror(rv));
        return nullptr;
    }

    rv = ngtcp2_conn_open_uni_stream(quic_conn, &qenc_stream_id, nullptr);
    if (rv != 0) {
        spdlog::error("Client: failed to open QPACK encoder stream: {}", ngtcp2_strerror(rv));
        return nullptr;
    }

    rv = ngtcp2_conn_open_uni_stream(quic_conn, &qdec_stream_id, nullptr);
    if (rv != 0) {
        spdlog::error("Client: failed to open QPACK decoder stream: {}", ngtcp2_strerror(rv));
        return nullptr;
    }

    rv = nghttp3_conn_bind_qpack_streams(session->conn_, qenc_stream_id, qdec_stream_id);
    if (rv != 0) {
        spdlog::error("Client: failed to bind QPACK streams: {}", nghttp3_strerror(rv));
        return nullptr;
    }

    spdlog::debug("HTTP/3 client session created (ctrl={}, qenc={}, qdec={})",
                  ctrl_stream_id, qenc_stream_id, qdec_stream_id);

    return session;
}

int64_t Http3ClientSession::submit_request(
    std::string_view method,
    std::string_view path,
    std::string_view authority,
    std::string_view body,
    const HeaderMap& extra_headers) {

    BodyChunkProvider provider;
    if (!body.empty()) {
        auto emitted = std::make_shared<bool>(false);
        provider = [data = std::string(body), emitted]() mutable -> std::optional<std::string> {
            if (*emitted) return std::nullopt;
            *emitted = true;
            return std::move(data);
        };
    }
    return submit_streaming_request(method, path, authority, std::move(provider), extra_headers);
}

int64_t Http3ClientSession::submit_streaming_request(
    std::string_view method,
    std::string_view path,
    std::string_view authority,
    BodyChunkProvider body,
    const HeaderMap& extra_headers) {

    // Open a new bidirectional stream
    int64_t stream_id = -1;
    int rv = ngtcp2_conn_open_bidi_stream(quic_conn_, &stream_id, nullptr);
    if (rv != 0) {
        spdlog::error("ngtcp2_conn_open_bidi_stream failed: {}", ngtcp2_strerror(rv));
        return -1;
    }

    // Build headers
    std::vector<nghttp3_nv> nva;
    nva.reserve(4 + extra_headers.size());

    auto push_nv = [&](std::string_view n, std::string_view v) {
        nva.push_back({
            reinterpret_cast<uint8_t*>(const_cast<char*>(n.data())),
            reinterpret_cast<uint8_t*>(const_cast<char*>(v.data())),
            n.size(), v.size(), 0
        });
    };

    push_nv(":method",    method);
    push_nv(":path",      path);
    push_nv(":scheme",    "https");
    push_nv(":authority", authority);

    // Extra headers
    for (const auto& entry : extra_headers.entries()) {
        push_nv(entry.name, entry.value);
    }

    // Store request body if any
    if (body) {
        RequestBody request_body;
        request_body.provider = std::move(body);
        request_bodies_[stream_id] = std::move(request_body);
    }

    // Data provider
    nghttp3_data_reader dr{};
    dr.read_data = on_read_data;

    rv = nghttp3_conn_submit_request(
        conn_, stream_id,
        nva.data(), nva.size(),
        request_bodies_.contains(stream_id) ? &dr : nullptr,
        nullptr); // stream user data

    if (rv != 0) {
        spdlog::error("nghttp3_conn_submit_request failed: {}", nghttp3_strerror(rv));
        request_bodies_.erase(stream_id);
        return -1;
    }

    spdlog::debug("HTTP/3 request submitted: {} {} (stream {})", method, path, stream_id);
    return stream_id;
}

void Http3ClientSession::cancel_stream(int64_t stream_id) {
    nghttp3_conn_shutdown_stream_write(conn_, stream_id);
    const int rv = nghttp3_conn_shutdown_stream_read(conn_, stream_id);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        spdlog::debug("nghttp3_conn_shutdown_stream_read (client) error: {}",
                      nghttp3_strerror(rv));
    }
    streams_.erase(stream_id);
    stream_read_offsets_.erase(stream_id);
    request_bodies_.erase(stream_id);
}

// ─── Forwarding methods (called by QuicClientConnection) ─────────────────────

int Http3ClientSession::on_stream_data(int64_t stream_id, uint64_t offset,
                                       const uint8_t* data, size_t datalen, bool fin) {
    if (datalen == 0 && !fin) return 0;

    uint64_t& cur = stream_read_offsets_[stream_id];

    if (offset + datalen <= cur) {
        if (fin) {
            nghttp3_conn_read_stream(conn_, stream_id, nullptr, 0, 1);
        }
        return 0;
    }
    if (offset < cur) {
        size_t skip = static_cast<size_t>(cur - offset);
        data    += skip;
        datalen -= skip;
        offset   = cur;
    }

    auto nread = nghttp3_conn_read_stream(conn_, stream_id, data, datalen, fin ? 1 : 0);
    if (nread < 0) {
        spdlog::debug("nghttp3_conn_read_stream (client) error: {}",
                      nghttp3_strerror(static_cast<int>(nread)));
        return -1;
    }

    cur = offset + datalen;

    ngtcp2_conn_extend_max_stream_offset(quic_conn_, stream_id,
                                         static_cast<uint64_t>(nread));
    ngtcp2_conn_extend_max_offset(quic_conn_, static_cast<uint64_t>(nread));
    return 0;
}

int Http3ClientSession::on_stream_close(int64_t stream_id, uint64_t app_error_code) {
    int rv = nghttp3_conn_close_stream(conn_, stream_id, app_error_code);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        spdlog::debug("nghttp3_conn_close_stream (client) error: {}", nghttp3_strerror(rv));
        return -1;
    }
    streams_.erase(stream_id);
    stream_read_offsets_.erase(stream_id);
    request_bodies_.erase(stream_id);
    return 0;
}

int Http3ClientSession::on_acked_stream_data(int64_t stream_id,
                                              uint64_t /*offset*/,
                                              uint64_t datalen) {
    int rv = nghttp3_conn_add_ack_offset(conn_, stream_id, datalen);
    if (rv != 0) {
        spdlog::debug("nghttp3_conn_add_ack_offset (client) error: {}", nghttp3_strerror(rv));
        return -1;
    }
    return 0;
}

int Http3ClientSession::on_extend_max_stream_data(int64_t stream_id) {
    int rv = nghttp3_conn_unblock_stream(conn_, stream_id);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        spdlog::debug("nghttp3_conn_unblock_stream (client) error: {}", nghttp3_strerror(rv));
        return -1;
    }
    return 0;
}

nghttp3_ssize Http3ClientSession::get_stream_data(int64_t* stream_id, int* fin,
                                                   ngtcp2_vec* vec, size_t veccnt) {
    auto* ngvec = reinterpret_cast<nghttp3_vec*>(vec);
    return nghttp3_conn_writev_stream(conn_, stream_id, fin, ngvec, veccnt);
}

void Http3ClientSession::add_write_offset(int64_t stream_id, size_t datalen) {
    int rv = nghttp3_conn_add_write_offset(conn_, stream_id, datalen);
    if (rv != 0) {
        spdlog::debug("nghttp3_conn_add_write_offset (client) error: {}",
                      nghttp3_strerror(rv));
    }
}

// ─── Stream helpers ──────────────────────────────────────────────────────────

Http3ClientSession::StreamState& Http3ClientSession::get_or_create(int64_t stream_id) {
    return streams_[stream_id];
}

Http3ClientSession::StreamState* Http3ClientSession::find(int64_t stream_id) {
    auto it = streams_.find(stream_id);
    return it != streams_.end() ? &it->second : nullptr;
}

// ─── nghttp3 callbacks ───────────────────────────────────────────────────────

int Http3ClientSession::on_recv_header(
    nghttp3_conn* /*conn*/, int64_t stream_id, int32_t /*token*/,
    nghttp3_rcbuf* name, nghttp3_rcbuf* value,
    uint8_t /*flags*/, void* conn_user_data, void* /*stream_user_data*/) {

    auto* session = static_cast<Http3ClientSession*>(conn_user_data);
    auto& state   = session->get_or_create(stream_id);

    auto name_vec  = nghttp3_rcbuf_get_buf(name);
    auto value_vec = nghttp3_rcbuf_get_buf(value);

    std::string_view name_sv(reinterpret_cast<const char*>(name_vec.base), name_vec.len);
    std::string_view value_sv(reinterpret_cast<const char*>(value_vec.base), value_vec.len);

    if (!name_sv.empty() && name_sv[0] == ':') {
        if (name_sv == ":status") {
            state.response.status_code = std::stoi(std::string(value_sv));
        }
    } else {
        state.response.headers.add(name_sv, value_sv);
    }
    return 0;
}

int Http3ClientSession::on_end_headers(
    nghttp3_conn* /*conn*/, int64_t stream_id, int /*fin*/,
    void* conn_user_data, void* /*stream_user_data*/) {

    auto* session = static_cast<Http3ClientSession*>(conn_user_data);
    auto* state   = session->find(stream_id);
    if (!state) return 0;

    state->headers_done = true;
    spdlog::debug("HTTP/3 client: response headers done (stream {}, status {})",
                  stream_id, state->response.status_code);
    return 0;
}

int Http3ClientSession::on_recv_data(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    const uint8_t* data, size_t datalen,
    void* conn_user_data, void* /*stream_user_data*/) {

    auto* session = static_cast<Http3ClientSession*>(conn_user_data);
    auto* state   = session->find(stream_id);
    if (!state) return 0;

    state->response.body.append(reinterpret_cast<const char*>(data), datalen);
    return 0;
}

int Http3ClientSession::on_end_stream(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    void* conn_user_data, void* /*stream_user_data*/) {

    auto* session = static_cast<Http3ClientSession*>(conn_user_data);
    auto* state   = session->find(stream_id);
    if (!state) return 0;

    spdlog::debug("HTTP/3 client: response complete (stream {}, status {}, body {} bytes)",
                  stream_id, state->response.status_code,
                  state->response.body.size());

    // Deliver the completed response
    if (session->handler_) {
        session->handler_(stream_id, std::move(state->response));
    }
    return 0;
}

int Http3ClientSession::on_acked_stream_data_cb(
    nghttp3_conn* /*conn*/, int64_t /*stream_id*/,
    uint64_t /*datalen*/, void* /*conn_user_data*/,
    void* /*stream_user_data*/) {
    return 0;
}

int Http3ClientSession::on_deferred_consume(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    size_t nconsumed, void* conn_user_data, void* /*stream_user_data*/) {

    auto* session = static_cast<Http3ClientSession*>(conn_user_data);
    ngtcp2_conn_extend_max_stream_offset(session->quic_conn_, stream_id, nconsumed);
    ngtcp2_conn_extend_max_offset(session->quic_conn_, nconsumed);
    return 0;
}

nghttp3_ssize Http3ClientSession::on_read_data(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    nghttp3_vec* vec, size_t /*veccnt*/,
    uint32_t* pflags,
    void* conn_user_data, void* /*stream_user_data*/) {

    auto* session = static_cast<Http3ClientSession*>(conn_user_data);
    auto it = session->request_bodies_.find(stream_id);
    if (it == session->request_bodies_.end()) {
        *pflags = NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }

    auto& rb      = it->second;
    if (rb.provided >= rb.data.size() && !rb.finished) {
        rb.data.clear();
        rb.provided = 0;
        if (rb.provider) {
            if (auto chunk = rb.provider(); chunk && !chunk->empty()) {
                rb.data = std::move(*chunk);
            } else {
                rb.finished = true;
            }
        } else {
            rb.finished = true;
        }
    }
    size_t remain = rb.data.size() - rb.provided;
    if (remain > 0) {
        vec[0].base = reinterpret_cast<uint8_t*>(rb.data.data() + rb.provided);
        vec[0].len  = remain;
        rb.provided += remain;
    }

    *pflags = rb.finished ? NGHTTP3_DATA_FLAG_EOF : NGHTTP3_DATA_FLAG_NONE;
    return remain > 0 ? 1 : 0;
}

} // namespace novaboot::http3
