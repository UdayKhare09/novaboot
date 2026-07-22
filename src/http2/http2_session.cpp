#include "novaboot/http2/http2_session.h"
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

namespace novaboot::http2 {

namespace {

nghttp2_ssize h2_data_source_read_callback(
    nghttp2_session* /*session*/, int32_t /*stream_id*/, uint8_t* buf,
    size_t length, uint32_t* data_flags,
    nghttp2_data_source* source, void* /*user_data*/) {
    
    auto* stream = static_cast<Http2Session::Http2Stream*>(source->ptr);
    if (stream->sse_channel) {
        while (stream->sse_current_offset >= stream->sse_current.size()) {
            stream->sse_current.clear();
            stream->sse_current_offset = 0;
            auto event = stream->sse_channel->take_next();
            if (!event) {
                if (stream->sse_channel->closed()) {
                    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                    return 0;
                }
                return NGHTTP2_ERR_DEFERRED;
            }
            stream->sse_current = std::move(*event);
        }
        const auto remaining = stream->sse_current.size() - stream->sse_current_offset;
        const auto to_write = std::min(length, remaining);
        std::memcpy(buf, stream->sse_current.data() + stream->sse_current_offset, to_write);
        stream->sse_current_offset += to_write;
        return static_cast<nghttp2_ssize>(to_write);
    }

    const auto& body = stream->response.body_str();
    size_t offset = stream->response.bytes_provided();
    size_t available = body.size() - offset;
    
    size_t to_write = std::min(length, available);
    if (to_write > 0) {
        std::memcpy(buf, body.data() + offset, to_write);
        stream->response.add_bytes_provided(to_write);
        offset += to_write;
    }
    
    if (offset >= body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    
    return static_cast<nghttp2_ssize>(to_write);
}

} // anonymous namespace

Http2Session::Http2Session(RequestHandler handler,
                           WebSocketConnectHandler websocket_handler,
                           websocket::Wakeup websocket_wakeup)
    : handler_(std::move(handler)),
      websocket_handler_(std::move(websocket_handler)),
      websocket_wakeup_(std::move(websocket_wakeup)) {
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);

    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_cb);

    nghttp2_session_server_new2(&session_, callbacks, this, nullptr);
    nghttp2_session_callbacks_del(callbacks);

    // Send SETTINGS frame at startup
    nghttp2_settings_entry iv[2] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        // RFC 8441: advertise extended CONNECT before accepting WebSockets
        // over an HTTP/2 stream.
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1}
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 2);
}

Http2Session::~Http2Session() {
    for (auto& [_, stream] : streams_) {
        if (stream.sse_channel) {
            stream.sse_channel->set_wakeup({});
            stream.sse_channel->close();
        }
    }
    if (session_) {
        nghttp2_session_del(session_);
    }
}

Http2Session::Http2Session(Http2Session&& other) noexcept
    : session_(other.session_),
      handler_(std::move(other.handler_)),
      websocket_handler_(std::move(other.websocket_handler_)),
      websocket_wakeup_(std::move(other.websocket_wakeup_)),
      streams_(std::move(other.streams_)),
      keep_alive_(other.keep_alive_),
      peer_address_(std::move(other.peer_address_)) {
    other.session_ = nullptr;
}

Http2Session& Http2Session::operator=(Http2Session&& other) noexcept {
    if (this != &other) {
        if (session_) nghttp2_session_del(session_);
        session_ = other.session_;
        handler_ = std::move(other.handler_);
        websocket_handler_ = std::move(other.websocket_handler_);
        websocket_wakeup_ = std::move(other.websocket_wakeup_);
        streams_ = std::move(other.streams_);
        keep_alive_ = other.keep_alive_;
        peer_address_ = std::move(other.peer_address_);
        other.session_ = nullptr;
    }
    return *this;
}

std::expected<std::vector<uint8_t>, int> Http2Session::feed_data(
    const std::vector<uint8_t>& decrypted_data,
    std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback) {

    if (!session_) {
        return std::unexpected(-1);
    }

    if (!decrypted_data.empty()) {
        ssize_t rv = nghttp2_session_mem_recv2(session_, decrypted_data.data(), decrypted_data.size());
        if (rv < 0) {
            spdlog::error("nghttp2_session_mem_recv2 failed: {}", nghttp2_strerror((int)rv));
            return std::unexpected((int)rv);
        }
    }

    return send_pending_frames(encrypt_callback);
}

std::vector<std::uint8_t> Http2Session::drain_websocket_outbound(
    std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)> encrypt_callback) {
    for (auto& [_, stream] : streams_) {
        if (!stream.websocket_active || !stream.websocket) continue;
        queue_websocket_outbound(stream, stream.websocket->drain_external_outbound());
    }
    return send_pending_frames(std::move(encrypt_callback));
}

std::vector<std::uint8_t> Http2Session::drain_outbound(
    std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)> encrypt_callback) {
    for (auto& [_, stream] : streams_) {
        if (stream.websocket_active && stream.websocket) {
            queue_websocket_outbound(stream, stream.websocket->drain_external_outbound());
        }
        if (stream.sse_channel) {
            const auto rv = nghttp2_session_resume_data(session_, stream.stream_id);
            if (rv != 0 && rv != NGHTTP2_ERR_INVALID_STATE) {
                spdlog::debug("nghttp2_session_resume_data failed: {}", nghttp2_strerror(rv));
            }
        }
    }
    return send_pending_frames(std::move(encrypt_callback));
}

std::vector<uint8_t> Http2Session::send_pending_frames(
    std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback) {
    
    std::vector<uint8_t> out;
    const uint8_t* data = nullptr;
    
    while (true) {
        ssize_t rv = nghttp2_session_mem_send2(session_, &data);
        if (rv > 0) {
            std::vector<uint8_t> raw(data, data + rv);
            std::vector<uint8_t> encrypted = encrypt_callback(raw);
            out.insert(out.end(), encrypted.begin(), encrypted.end());
        } else if (rv == 0) {
            break;
        } else {
            spdlog::error("nghttp2_session_mem_send2 failed: {}", nghttp2_strerror((int)rv));
            break;
        }
    }
    
    return out;
}

int Http2Session::on_begin_headers_cb(nghttp2_session* /*session*/,
                                     const nghttp2_frame* frame,
                                     void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        int32_t stream_id = frame->hd.stream_id;
        Http2Stream stream{};
        stream.stream_id = stream_id;
        stream.request.set_peer_address(self->peer_address_);
        self->streams_[stream_id] = std::move(stream);
    }
    return 0;
}

int Http2Session::on_header_cb(nghttp2_session* /*session*/,
                              const nghttp2_frame* frame,
                              const uint8_t* name, size_t namelen,
                              const uint8_t* value, size_t valuelen,
                              uint8_t /*flags*/, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    int32_t stream_id = frame->hd.stream_id;
    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) {
        return 0;
    }

    std::string_view name_sv(reinterpret_cast<const char*>(name), namelen);
    std::string_view value_sv(reinterpret_cast<const char*>(value), valuelen);

    if (name_sv.starts_with(':')) {
        if (name_sv == ":method") {
            it->second.request.set_method(value_sv);
        } else if (name_sv == ":path") {
            it->second.request.set_path(value_sv);
        } else if (name_sv == ":scheme") {
            it->second.request.set_scheme(value_sv);
        } else if (name_sv == ":authority") {
            it->second.request.set_authority(value_sv);
        } else if (name_sv == ":protocol" && value_sv == "websocket") {
            it->second.extended_websocket_connect = true;
        }
    } else {
        it->second.request.headers().add(name_sv, value_sv);
    }

    return 0;
}

int Http2Session::on_data_chunk_recv_cb(nghttp2_session* /*session*/, uint8_t /*flags*/,
                                       int32_t stream_id, const uint8_t* data,
                                       size_t len, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it != self->streams_.end()) {
        auto& stream = it->second;
        if (stream.websocket_active && stream.websocket) {
            const auto result = stream.websocket->feed({data, len});
            // A protocol error queues its RFC 6455 close frame.  Keep the
            // HTTP/2 stream alive long enough for nghttp2 to deliver it.
            (void)result;
            self->queue_websocket_outbound(stream, stream.websocket->take_outbound());
        } else {
            stream.request.append_body(data, len);
        }
    }
    return 0;
}

int Http2Session::on_frame_recv_cb(nghttp2_session* /*session*/,
                                  const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    if (frame->hd.type == NGHTTP2_HEADERS) {
        const auto it = self->streams_.find(frame->hd.stream_id);
        if (it != self->streams_.end() &&
            (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) &&
            it->second.extended_websocket_connect) {
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                it->second.response.status(400).body("WebSocket CONNECT stream must remain open");
                self->handle_stream_request(it->second);
            } else {
                self->handle_websocket_connect(it->second);
            }
            return 0;
        }
    }

    if (frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            int32_t stream_id = frame->hd.stream_id;
            auto it = self->streams_.find(stream_id);
            if (it != self->streams_.end()) {
                if (it->second.websocket_active) return 0;
                it->second.request.mark_headers_complete();
                it->second.request.mark_body_complete();
                it->second.request_complete = true;
                self->handle_stream_request(it->second);
            }
        }
    }
    return 0;
}

int Http2Session::on_stream_close_cb(nghttp2_session* /*session*/, int32_t stream_id,
                                    uint32_t /*error_code*/, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    if (const auto it = self->streams_.find(stream_id); it != self->streams_.end()) {
        if (it->second.sse_channel) {
            it->second.sse_channel->set_wakeup({});
            it->second.sse_channel->close();
        }
        self->streams_.erase(it);
    }
    return 0;
}

void Http2Session::handle_stream_request(Http2Stream& stream) {
    // Execute handler
    handler_(stream.request, stream.response);

    if (const auto& channel = stream.response.event_stream()) {
        stream.sse_channel = channel;
        stream.sse_channel->set_wakeup(websocket_wakeup_);
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

    std::string status_str = std::to_string(stream.response.status_code());
    add_nv(":status", status_str);
    add_nv("alt-svc", "h3=\":4433\"; ma=86400");

    for (const auto& entry : stream.response.headers().entries()) {
        std::string name_lower = entry.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        if (name_lower != "alt-svc") {
            add_nv(entry.name, entry.value);
        }
    }

    if (!stream.sse_channel && !stream.response.headers().has("content-length")) {
        std::string content_length_str = std::to_string(stream.response.body_size());
        add_nv("content-length", content_length_str);
    }

    nghttp2_data_provider2 data_prd;
    data_prd.source.ptr = &stream;
    data_prd.read_callback = h2_data_source_read_callback;

    int rv = nghttp2_submit_response2(session_, stream.stream_id, nvs.data(), nvs.size(), &data_prd);
    if (rv != 0) {
        spdlog::error("nghttp2_submit_response2 failed on stream {}: {}", stream.stream_id, nghttp2_strerror(rv));
    }
}

void Http2Session::handle_websocket_connect(Http2Stream& stream) {
    if (stream.request.method() != "CONNECT") {
        stream.response.status(400).body("WebSocket extended CONNECT requires CONNECT");
        handle_stream_request(stream);
        return;
    }

    const auto result = websocket_handler_
        ? websocket_handler_(stream.request)
        : WebSocketConnectResult{};
    if (!result.matched) {
        stream.response.status(404).body("WebSocket endpoint not found");
        handle_stream_request(stream);
        return;
    }
    if (!result.accepted || !result.handler) {
        stream.response.status(result.rejection_status).body(result.rejection_body);
        handle_stream_request(stream);
        return;
    }

    stream.websocket_backpressure = std::make_shared<websocket::TransportBackpressure>(
        result.handler->limits.max_pending_send_bytes);
    stream.websocket.emplace(*result.handler, result.principal, websocket_wakeup_,
                             stream.websocket_backpressure);
    stream.websocket_active = true;

    const std::string status = "200";
    std::array<nghttp2_nv, 2> headers{};
    auto& status_header = headers[0];
    status_header.name = const_cast<std::uint8_t*>(
        reinterpret_cast<const std::uint8_t*>(":status"));
    status_header.value = const_cast<std::uint8_t*>(
        reinterpret_cast<const std::uint8_t*>(status.data()));
    status_header.namelen = 7;
    status_header.valuelen = status.size();
    status_header.flags = NGHTTP2_NV_FLAG_NONE;
    std::size_t header_count = 1;
    if (!result.subprotocol.empty()) {
        auto& protocol_header = headers[header_count++];
        protocol_header.name = const_cast<std::uint8_t*>(
            reinterpret_cast<const std::uint8_t*>("sec-websocket-protocol"));
        protocol_header.value = const_cast<std::uint8_t*>(
            reinterpret_cast<const std::uint8_t*>(result.subprotocol.data()));
        protocol_header.namelen = sizeof("sec-websocket-protocol") - 1;
        protocol_header.valuelen = result.subprotocol.size();
        protocol_header.flags = NGHTTP2_NV_FLAG_NONE;
    }
    const int rv = nghttp2_submit_headers(
        session_, NGHTTP2_FLAG_NONE, stream.stream_id, nullptr, headers.data(), header_count, nullptr);
    if (rv != 0) {
        spdlog::error("nghttp2_submit_headers failed for WebSocket stream {}: {}",
                      stream.stream_id, nghttp2_strerror(rv));
        stream.websocket.reset();
        stream.websocket_active = false;
        return;
    }
    queue_websocket_outbound(stream, stream.websocket->take_outbound());
}

void Http2Session::queue_websocket_outbound(Http2Stream& stream,
                                             std::vector<std::uint8_t> data) {
    if (data.empty() || !stream.websocket_active) return;
    stream.websocket_outbound.push_back(std::move(data));
    submit_websocket_data(stream);
}

void Http2Session::submit_websocket_data(Http2Stream& stream) {
    if (!session_ || stream.websocket_data_submission_active ||
        stream.websocket_outbound.empty()) {
        return;
    }
    nghttp2_data_provider2 provider{};
    provider.source.ptr = &stream;
    provider.read_callback = websocket_data_source_read_callback;
    const int rv = nghttp2_submit_data2(session_, NGHTTP2_FLAG_NONE,
                                        stream.stream_id, &provider);
    if (rv != 0) {
        spdlog::error("nghttp2_submit_data failed for WebSocket stream {}: {}",
                      stream.stream_id, nghttp2_strerror(rv));
        return;
    }
    stream.websocket_data_submission_active = true;
}

nghttp2_ssize Http2Session::websocket_data_source_read_callback(
    nghttp2_session* /*session*/, int32_t /*stream_id*/, std::uint8_t* buf,
    std::size_t length, std::uint32_t* data_flags,
    nghttp2_data_source* source, void* /*user_data*/) {
    auto* stream = static_cast<Http2Stream*>(source->ptr);
    if (!stream || stream->websocket_outbound.empty()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
        if (stream) stream->websocket_data_submission_active = false;
        return 0;
    }

    auto& current = stream->websocket_outbound.front();
    const auto available = current.size() - stream->websocket_outbound_offset;
    const auto to_write = std::min(length, available);
    std::memcpy(buf, current.data() + stream->websocket_outbound_offset, to_write);
    stream->websocket_outbound_offset += to_write;
    if (stream->websocket_backpressure) {
        stream->websocket_backpressure->release(to_write);
    }
    if (stream->websocket_outbound_offset == current.size()) {
        stream->websocket_outbound.pop_front();
        stream->websocket_outbound_offset = 0;
    }
    if (stream->websocket_outbound.empty()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
        stream->websocket_data_submission_active = false;
    }
    return static_cast<nghttp2_ssize>(to_write);
}

} // namespace novaboot::http2
