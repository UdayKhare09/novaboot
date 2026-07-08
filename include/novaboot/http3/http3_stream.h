#pragma once

#include <cstdint>
#include <memory>

#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::http3 {

/// Individual HTTP/3 request/response stream.
///
/// Each client request opens a new bidirectional QUIC stream.
/// This class owns the Request (populated by callbacks) and
/// Response (populated by the route handler).
class Http3Stream {
public:
    explicit Http3Stream(int64_t stream_id);
    ~Http3Stream() = default;

    // Non-copyable, movable
    Http3Stream(const Http3Stream&) = delete;
    Http3Stream& operator=(const Http3Stream&) = delete;
    Http3Stream(Http3Stream&&) = default;
    Http3Stream& operator=(Http3Stream&&) = default;

    [[nodiscard]] int64_t stream_id() const noexcept { return stream_id_; }

    [[nodiscard]] Request& request() noexcept { return request_; }
    [[nodiscard]] const Request& request() const noexcept { return request_; }

    [[nodiscard]] Response& response() noexcept { return response_; }
    [[nodiscard]] const Response& response() const noexcept {
        return response_;
    }

    /// Mark headers as fully received — ready for routing
    void mark_headers_received() noexcept { headers_received_ = true; }
    [[nodiscard]] bool headers_received() const noexcept {
        return headers_received_;
    }

    /// Mark request as fully received (no more body data)
    void mark_request_complete() noexcept { request_complete_ = true; }
    [[nodiscard]] bool request_complete() const noexcept {
        return request_complete_;
    }

    /// Mark response as submitted to nghttp3
    void mark_response_submitted() noexcept { response_submitted_ = true; }
    [[nodiscard]] bool response_submitted() const noexcept {
        return response_submitted_;
    }

private:
    int64_t  stream_id_;
    Request  request_;
    Response response_;

    bool headers_received_   = false;
    bool request_complete_   = false;
    bool response_submitted_ = false;
};

} // namespace novaboot::http3
