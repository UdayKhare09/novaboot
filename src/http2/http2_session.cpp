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

Http2Session::Http2Session(RequestHandler handler) : handler_(std::move(handler)) {
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
    nghttp2_settings_entry iv[1] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);
}

Http2Session::~Http2Session() {
    if (session_) {
        nghttp2_session_del(session_);
    }
}

Http2Session::Http2Session(Http2Session&& other) noexcept
    : session_(other.session_),
      handler_(std::move(other.handler_)),
      streams_(std::move(other.streams_)),
      keep_alive_(other.keep_alive_) {
    other.session_ = nullptr;
}

Http2Session& Http2Session::operator=(Http2Session&& other) noexcept {
    if (this != &other) {
        if (session_) nghttp2_session_del(session_);
        session_ = other.session_;
        handler_ = std::move(other.handler_);
        streams_ = std::move(other.streams_);
        keep_alive_ = other.keep_alive_;
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
        self->streams_[stream_id] = Http2Stream{stream_id, http3::Request(), http3::Response(), false};
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
        it->second.request.append_body(data, len);
    }
    return 0;
}

int Http2Session::on_frame_recv_cb(nghttp2_session* /*session*/,
                                  const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    if (frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            int32_t stream_id = frame->hd.stream_id;
            auto it = self->streams_.find(stream_id);
            if (it != self->streams_.end()) {
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
    self->streams_.erase(stream_id);
    return 0;
}

void Http2Session::handle_stream_request(Http2Stream& stream) {
    // Execute handler
    handler_(stream.request, stream.response);

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
        if (name_lower != "content-length" && name_lower != "alt-svc") {
            add_nv(entry.name, entry.value);
        }
    }

    std::string content_length_str = std::to_string(stream.response.body_size());
    add_nv("content-length", content_length_str);

    nghttp2_data_provider2 data_prd;
    data_prd.source.ptr = &stream;
    data_prd.read_callback = h2_data_source_read_callback;

    int rv = nghttp2_submit_response2(session_, stream.stream_id, nvs.data(), nvs.size(), &data_prd);
    if (rv != 0) {
        spdlog::error("nghttp2_submit_response2 failed on stream {}: {}", stream.stream_id, nghttp2_strerror(rv));
    }
}

} // namespace novaboot::http2
