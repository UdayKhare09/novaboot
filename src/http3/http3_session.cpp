#include "novaboot/http3/http3_session.h"

#include <cstring>
#include <format>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace novaboot::http3 {

Http3Session::~Http3Session() {
    if (conn_) {
        nghttp3_conn_del(conn_);
    }
}

std::unique_ptr<Http3Session> Http3Session::create(
    ngtcp2_conn* quic_conn,
    RequestHandler handler) {

    auto session = std::unique_ptr<Http3Session>(new Http3Session());
    session->quic_conn_ = quic_conn;
    session->handler_   = std::move(handler);

    // Setup nghttp3 callbacks
    nghttp3_callbacks callbacks{};
    callbacks.recv_header       = on_recv_header;
    callbacks.end_headers       = on_end_headers;
    callbacks.recv_data         = on_recv_data;
    callbacks.end_stream        = on_end_stream;
    callbacks.acked_stream_data = on_acked_stream_data_cb;
    callbacks.deferred_consume  = on_deferred_consume;

    // Settings
    nghttp3_settings settings;
    nghttp3_settings_default(&settings);

    // Create the nghttp3 server connection
    int rv = nghttp3_conn_server_new(
        &session->conn_,
        &callbacks,
        &settings,
        nghttp3_mem_default(),
        session.get()); // user_data

    if (rv != 0) {
        spdlog::error("nghttp3_conn_server_new failed: {}", nghttp3_strerror(rv));
        return nullptr;
    }

    // Bind control and QPACK streams
    // ngtcp2 opens unidirectional streams for HTTP/3 control:
    //   stream 3: control stream
    //   stream 7: QPACK encoder
    //   stream 11: QPACK decoder
    int64_t ctrl_stream_id, qenc_stream_id, qdec_stream_id;

    rv = ngtcp2_conn_open_uni_stream(quic_conn, &ctrl_stream_id, nullptr);
    if (rv != 0) {
        spdlog::error("Failed to open control stream: {}", ngtcp2_strerror(rv));
        return nullptr;
    }
    rv = nghttp3_conn_bind_control_stream(session->conn_, ctrl_stream_id);
    if (rv != 0) {
        spdlog::error("Failed to bind control stream: {}", nghttp3_strerror(rv));
        return nullptr;
    }

    rv = ngtcp2_conn_open_uni_stream(quic_conn, &qenc_stream_id, nullptr);
    if (rv != 0) {
        spdlog::error("Failed to open QPACK encoder stream: {}", ngtcp2_strerror(rv));
        return nullptr;
    }

    rv = ngtcp2_conn_open_uni_stream(quic_conn, &qdec_stream_id, nullptr);
    if (rv != 0) {
        spdlog::error("Failed to open QPACK decoder stream: {}", ngtcp2_strerror(rv));
        return nullptr;
    }

    rv = nghttp3_conn_bind_qpack_streams(
        session->conn_, qenc_stream_id, qdec_stream_id);
    if (rv != 0) {
        spdlog::error("Failed to bind QPACK streams: {}", nghttp3_strerror(rv));
        return nullptr;
    }

    spdlog::debug("HTTP/3 session created (ctrl={}, qenc={}, qdec={})",
                  ctrl_stream_id, qenc_stream_id, qdec_stream_id);

    return session;
}

int Http3Session::on_stream_data(int64_t stream_id, const uint8_t* data,
                                 size_t datalen, bool fin) {
    auto nread = nghttp3_conn_read_stream(
        conn_, stream_id, data, datalen, fin ? 1 : 0);

    if (nread < 0) {
        spdlog::debug("nghttp3_conn_read_stream error: {}",
                      nghttp3_strerror(static_cast<int>(nread)));
        return -1;
    }

    // Tell ngtcp2 how much data nghttp3 consumed
    ngtcp2_conn_extend_max_stream_offset(
        quic_conn_, stream_id, static_cast<uint64_t>(nread));
    ngtcp2_conn_extend_max_offset(
        quic_conn_, static_cast<uint64_t>(nread));

    return 0;
}

int Http3Session::on_stream_close(int64_t stream_id,
                                  uint64_t app_error_code) {
    int rv = nghttp3_conn_close_stream(conn_, stream_id, app_error_code);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        spdlog::debug("nghttp3_conn_close_stream error: {}",
                      nghttp3_strerror(rv));
        return -1;
    }

    // Clean up the stream
    streams_.erase(stream_id);
    return 0;
}

int Http3Session::on_acked_stream_data(int64_t stream_id,
                                       uint64_t /*offset*/,
                                       uint64_t datalen) {
    int rv = nghttp3_conn_add_ack_offset(conn_, stream_id, datalen);
    if (rv != 0) {
        spdlog::debug("nghttp3_conn_add_ack_offset error: {}",
                      nghttp3_strerror(rv));
        return -1;
    }
    return 0;
}

int Http3Session::on_extend_max_stream_data(int64_t stream_id) {
    int rv = nghttp3_conn_unblock_stream(conn_, stream_id);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        spdlog::debug("nghttp3_conn_unblock_stream error: {}",
                      nghttp3_strerror(rv));
        return -1;
    }
    return 0;
}

nghttp3_ssize Http3Session::get_stream_data(int64_t* stream_id, int* fin,
                                            ngtcp2_vec* vec, size_t veccnt) {
    // Convert ngtcp2_vec to nghttp3_vec (they have the same layout)
    auto* ngvec = reinterpret_cast<nghttp3_vec*>(vec);

    return nghttp3_conn_writev_stream(
        conn_, stream_id, fin, ngvec, veccnt);
}

void Http3Session::add_write_offset(int64_t stream_id, size_t datavcnt,
                                    ngtcp2_vec* datav) {
    uint64_t total = 0;
    for (size_t i = 0; i < datavcnt; ++i) {
        total += datav[i].len;
    }

    int rv = nghttp3_conn_add_write_offset(conn_, stream_id,
                                           static_cast<size_t>(total));
    if (rv != 0) {
        spdlog::debug("nghttp3_conn_add_write_offset error: {}",
                      nghttp3_strerror(rv));
    }
}

int Http3Session::submit_response(Http3Stream& stream) {
    if (stream.response_submitted()) {
        return 0;
    }

    auto& res = stream.response();

    // Ensure content-length is set
    if (!res.headers().has("content-length") && res.body_size() > 0) {
        res.headers().set("content-length", std::to_string(res.body_size()));
    }

    // Build nghttp3 name-value pairs
    std::vector<nghttp3_nv> nva;
    nva.reserve(res.headers().size() + 1);

    // Status pseudo-header
    auto status_str = std::to_string(res.status_code());
    nva.push_back({
        reinterpret_cast<uint8_t*>(const_cast<char*>(":status")),
        reinterpret_cast<uint8_t*>(status_str.data()),
        7, // strlen(":status")
        status_str.size(),
        NGHTTP3_NV_FLAG_NO_COPY_NAME
    });

    // Regular headers
    for (const auto& entry : res.headers().entries()) {
        nva.push_back({
            reinterpret_cast<uint8_t*>(const_cast<char*>(entry.name.data())),
            reinterpret_cast<uint8_t*>(const_cast<char*>(entry.value.data())),
            entry.name.size(),
            entry.value.size(),
            0
        });
    }

    // Data provider (for response body)
    nghttp3_data_reader dr{};
    dr.read_data = on_read_data;

    int rv = nghttp3_conn_submit_response(
        conn_, stream.stream_id(),
        nva.data(), nva.size(),
        res.body_size() > 0 ? &dr : nullptr);

    if (rv != 0) {
        spdlog::error("nghttp3_conn_submit_response error: {}",
                      nghttp3_strerror(rv));
        return rv;
    }

    stream.mark_response_submitted();
    spdlog::debug("Response submitted for stream {} (status={})",
                  stream.stream_id(), res.status_code());

    return 0;
}

Http3Stream* Http3Session::find_stream(int64_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return it->second.get();
    }
    return nullptr;
}

Http3Stream& Http3Session::get_or_create_stream(int64_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return *it->second;
    }

    auto stream = std::make_unique<Http3Stream>(stream_id);
    auto& ref = *stream;
    streams_[stream_id] = std::move(stream);

    // Set stream user_data in nghttp3
    nghttp3_conn_set_stream_user_data(conn_, stream_id, &ref);

    return ref;
}

// ─── nghttp3 callback implementations ───────────────────────────────────────

int Http3Session::on_recv_header(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    int32_t /*token*/,
    nghttp3_rcbuf* name, nghttp3_rcbuf* value,
    uint8_t /*flags*/, void* conn_user_data,
    void* /*stream_user_data*/) {

    auto* session = static_cast<Http3Session*>(conn_user_data);
    auto& stream = session->get_or_create_stream(stream_id);

    auto name_vec  = nghttp3_rcbuf_get_buf(name);
    auto value_vec = nghttp3_rcbuf_get_buf(value);

    std::string_view name_sv(
        reinterpret_cast<const char*>(name_vec.base), name_vec.len);
    std::string_view value_sv(
        reinterpret_cast<const char*>(value_vec.base), value_vec.len);

    // Handle pseudo-headers
    if (!name_sv.empty() && name_sv[0] == ':') {
        if (name_sv == ":method") {
            stream.request().set_method(value_sv);
        } else if (name_sv == ":path") {
            stream.request().set_path(value_sv);
        } else if (name_sv == ":scheme") {
            stream.request().set_scheme(value_sv);
        } else if (name_sv == ":authority") {
            stream.request().set_authority(value_sv);
        }
    } else {
        stream.request().headers().add(name_sv, value_sv);
    }

    return 0;
}

int Http3Session::on_end_headers(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    int /*fin*/, void* conn_user_data,
    void* /*stream_user_data*/) {

    auto* session = static_cast<Http3Session*>(conn_user_data);
    auto* stream = session->find_stream(stream_id);
    if (!stream) return 0;

    stream->request().mark_headers_complete();
    stream->mark_headers_received();

    spdlog::debug("Request headers complete: {} {} (stream {})",
                  stream->request().method(),
                  stream->request().path(),
                  stream_id);

    return 0;
}

int Http3Session::on_recv_data(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    const uint8_t* data, size_t datalen,
    void* conn_user_data,
    void* /*stream_user_data*/) {

    auto* session = static_cast<Http3Session*>(conn_user_data);
    auto* stream = session->find_stream(stream_id);
    if (!stream) return 0;

    stream->request().append_body(data, datalen);
    return 0;
}

int Http3Session::on_end_stream(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    void* conn_user_data,
    void* /*stream_user_data*/) {

    auto* session = static_cast<Http3Session*>(conn_user_data);
    auto* stream = session->find_stream(stream_id);
    if (!stream) return 0;

    stream->request().mark_body_complete();
    stream->mark_request_complete();

    spdlog::debug("Request complete: {} {} (stream {}, body {} bytes)",
                  stream->request().method(),
                  stream->request().path(),
                  stream_id,
                  stream->request().content_length());

    // Route the request
    if (session->handler_) {
        session->handler_(*stream);

        // Submit the response if the handler has set one
        if (!stream->response_submitted()) {
            session->submit_response(*stream);
        }
    }

    return 0;
}

int Http3Session::on_acked_stream_data_cb(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    uint64_t datalen,
    void* conn_user_data,
    void* /*stream_user_data*/) {

    auto* session = static_cast<Http3Session*>(conn_user_data);
    auto* stream = session->find_stream(stream_id);
    if (!stream) return 0;

    stream->response().add_bytes_sent(static_cast<std::size_t>(datalen));
    return 0;
}

int Http3Session::on_deferred_consume(
    nghttp3_conn* /*conn*/, int64_t stream_id,
    size_t nconsumed,
    void* conn_user_data,
    void* /*stream_user_data*/) {

    auto* session = static_cast<Http3Session*>(conn_user_data);

    // Extend QUIC flow control
    ngtcp2_conn_extend_max_stream_offset(
        session->quic_conn_, stream_id, nconsumed);
    ngtcp2_conn_extend_max_offset(session->quic_conn_, nconsumed);

    return 0;
}

nghttp3_ssize Http3Session::on_read_data(
    nghttp3_conn* /*conn*/, int64_t /*stream_id*/,
    nghttp3_vec* vec, size_t /*veccnt*/,
    uint32_t* pflags,
    void* /*conn_user_data*/,
    void* stream_user_data) {

    auto* stream = static_cast<Http3Stream*>(stream_user_data);
    if (!stream) {
        *pflags = NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }

    auto& res = stream->response();
    auto body = res.body_data();
    auto sent = res.bytes_sent();
    auto remaining = body.size() - sent;

    if (remaining > 0) {
        vec[0].base = const_cast<uint8_t*>(
            reinterpret_cast<const uint8_t*>(body.data() + sent));
        vec[0].len = remaining;
    }

    // This is the last chunk
    *pflags = NGHTTP3_DATA_FLAG_EOF;

    return remaining > 0 ? 1 : 0;
}

} // namespace novaboot::http3
