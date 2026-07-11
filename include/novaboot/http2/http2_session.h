#pragma once

#include <nghttp2/nghttp2.h>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <expected>
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::http2 {

class Http2Session {
public:
    using RequestHandler = std::function<void(http3::Request&, http3::Response&)>;

    struct Http2Stream {
        int32_t stream_id;
        http3::Request request;
        http3::Response response;
        bool request_complete = false;
    };

    explicit Http2Session(RequestHandler handler);
    ~Http2Session();

    Http2Session(const Http2Session&) = delete;
    Http2Session& operator=(const Http2Session&) = delete;
    Http2Session(Http2Session&&) noexcept;
    Http2Session& operator=(Http2Session&&) noexcept;

    /// Feed decrypted incoming network bytes.
    /// Returns any encrypted response data that should be sent to the network.
    std::expected<std::vector<uint8_t>, int> feed_data(
        const std::vector<uint8_t>& decrypted_data,
        std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback);

    [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }

private:
    static int on_begin_headers_cb(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data);
    static int on_header_cb(nghttp2_session* session,
                            const nghttp2_frame* frame,
                            const uint8_t* name, size_t namelen,
                            const uint8_t* value, size_t valuelen,
                            uint8_t flags, void* user_data);
    static int on_data_chunk_recv_cb(nghttp2_session* session, uint8_t flags,
                                     int32_t stream_id, const uint8_t* data,
                                     size_t len, void* user_data);
    static int on_frame_recv_cb(nghttp2_session* session,
                                const nghttp2_frame* frame, void* user_data);
    static int on_stream_close_cb(nghttp2_session* session, int32_t stream_id,
                                  uint32_t error_code, void* user_data);

    void handle_stream_request(Http2Stream& stream);
    std::vector<uint8_t> send_pending_frames(
        std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback);

    nghttp2_session* session_ = nullptr;
    RequestHandler handler_;
    std::unordered_map<int32_t, Http2Stream> streams_;
    bool keep_alive_ = true;
};

} // namespace novaboot::http2
