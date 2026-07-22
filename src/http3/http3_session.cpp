#include "novaboot/http3/http3_session.h"

#include <cstring>
#include <format>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace novaboot::http3 {

Http3Session::~Http3Session() {
    for (auto& [_, stream] : streams_) {
        if (stream->sse_channel()) {
            stream->sse_channel()->set_wakeup({});
            stream->sse_channel()->close();
        }
    }
    if (conn_) {
        nghttp3_conn_del(conn_);
    }
}

std::unique_ptr<Http3Session> Http3Session::create(
    ngtcp2_conn* quic_conn,
    RequestHandler handler,
    SseWakeup sse_wakeup) {

    auto session = std::unique_ptr<Http3Session>(new Http3Session());
    session->quic_conn_ = quic_conn;
    session->handler_   = std::move(handler);
    session->sse_wakeup_ = std::move(sse_wakeup);

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
    settings.qpack_max_dtable_capacity = 4096;
    settings.qpack_encoder_max_dtable_capacity = 4096;
    settings.qpack_blocked_streams = 100;

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

    nghttp3_conn_set_max_client_streams_bidi(session->conn_, 100);

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

int Http3Session::on_stream_data(int64_t stream_id, uint64_t offset, const uint8_t* data,
                                 size_t datalen, bool fin) {
    if (datalen == 0 && !fin) {
        return 0;
    }

    uint64_t& current_read_offset = stream_read_offsets_[stream_id];

    spdlog::debug("on_stream_data: stream_id={}, offset={}, datalen={}, fin={}, current_read_offset={}",
                  stream_id, offset, datalen, fin, current_read_offset);

    if (offset + datalen <= current_read_offset) {
        // This entire chunk is duplicate data we already processed.
        if (fin) {
            auto nread = nghttp3_conn_read_stream(conn_, stream_id, nullptr, 0, 1);
            if (nread < 0) {
                spdlog::debug("nghttp3_conn_read_stream error: {}",
                              nghttp3_strerror(static_cast<int>(nread)));
                return -1;
            }
        }
        return 0;
    }

    if (offset < current_read_offset) {
        // Skip duplicate bytes at the start of the chunk
        size_t skip = static_cast<size_t>(current_read_offset - offset);
        data += skip;
        datalen -= skip;
        offset = current_read_offset;
    }

    auto nread = nghttp3_conn_read_stream(
        conn_, stream_id, data, datalen, fin ? 1 : 0);

    spdlog::debug("on_stream_data: stream_id={}, nread={}", stream_id, nread);

    if (nread < 0) {
        spdlog::debug("nghttp3_conn_read_stream error: {}",
                      nghttp3_strerror(static_cast<int>(nread)));
        return -1;
    }

    // Update the read offset to match what we actually read/provided
    current_read_offset = offset + datalen;

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

    // Clean up the stream and invalidate any producer retained by application code.
    if (const auto it = streams_.find(stream_id); it != streams_.end()) {
        if (it->second->sse_channel()) {
            it->second->sse_channel()->set_wakeup({});
            it->second->sse_channel()->close();
        }
        streams_.erase(it);
    }
    stream_read_offsets_.erase(stream_id);
    return 0;
}

void Http3Session::resume_sse_stream(int64_t stream_id) {
    const auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second->sse_channel()) return;
    const auto rv = nghttp3_conn_resume_stream(conn_, stream_id);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        spdlog::debug("nghttp3_conn_resume_stream error: {}", nghttp3_strerror(rv));
    }
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

void Http3Session::add_write_offset(int64_t stream_id, size_t datalen) {
    int rv = nghttp3_conn_add_write_offset(conn_, stream_id, datalen);
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
    const auto& sse_channel = res.event_stream();

    // Ensure content-length is set
    if (!sse_channel && !res.headers().has("content-length") && res.body_size() > 0) {
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
        (res.body_size() > 0 || sse_channel) ? &dr : nullptr);

    if (rv != 0) {
        spdlog::error("nghttp3_conn_submit_response error: {}",
                      nghttp3_strerror(rv));
        return rv;
    }

    stream.mark_response_submitted();
    if (sse_channel) {
        stream.sse_channel() = sse_channel;
        stream.sse_channel()->set_wakeup([this, stream_id = stream.stream_id()] {
            if (sse_wakeup_) sse_wakeup_(stream_id);
        });
    }
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
    stream->request().set_peer_address(peer_address_);
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
    if (stream->sse_channel() && stream->sse_inflight_ack_remaining() > 0) {
        auto& remaining = stream->sse_inflight_ack_remaining();
        remaining = datalen >= remaining ? 0 : remaining - static_cast<std::size_t>(datalen);
        if (remaining == 0) {
            stream->sse_inflight().clear();
            session->resume_sse_stream(stream_id);
        }
    }
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
    if (stream->sse_channel()) {
        if (stream->sse_inflight_ack_remaining() > 0) return NGHTTP3_ERR_WOULDBLOCK;
        auto event = stream->sse_channel()->take_next();
        if (!event) {
            if (stream->sse_channel()->closed()) {
                *pflags = NGHTTP3_DATA_FLAG_EOF;
                return 0;
            }
            return NGHTTP3_ERR_WOULDBLOCK;
        }
        stream->sse_inflight() = std::move(*event);
        stream->sse_inflight_ack_remaining() = stream->sse_inflight().size();
        vec[0].base = reinterpret_cast<uint8_t*>(stream->sse_inflight().data());
        vec[0].len = stream->sse_inflight().size();
        return 1;
    }
    auto body = res.body_data();
    auto provided = res.bytes_provided();
    auto remaining = body.size() - provided;

    spdlog::debug("on_read_data: stream_id={}, body_size={}, provided={}, remaining={}",
                  stream->stream_id(), body.size(), provided, remaining);

    if (remaining > 0) {
        vec[0].base = const_cast<uint8_t*>(
            reinterpret_cast<const uint8_t*>(body.data() + provided));
        vec[0].len = remaining;
        res.add_bytes_provided(remaining);
    }

    // This is the last chunk
    *pflags = NGHTTP3_DATA_FLAG_EOF;

    return remaining > 0 ? 1 : 0;
}

} // namespace novaboot::http3
