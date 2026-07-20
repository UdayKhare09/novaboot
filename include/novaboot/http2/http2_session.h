#pragma once

#include <nghttp2/nghttp2.h>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <expected>
#include <deque>
#include <optional>
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/websocket/websocket.h"

namespace novaboot::http2 {

class Http2Session {
public:
    using RequestHandler = std::function<void(http3::Request&, http3::Response&)>;

    /// The routing result for an RFC 8441 extended CONNECT request.  Keeping
    /// this separate from HTTP/1.1's Upgrade result lets both transports use
    /// the same websocket::Handler without leaking HTTP/1.1 semantics into
    /// HTTP/2.
    struct WebSocketConnectResult {
        bool matched = false;
        bool accepted = false;
        int rejection_status = 403;
        std::string rejection_body = "Forbidden";
        std::optional<websocket::Handler> handler;
        std::string principal;

        [[nodiscard]] static WebSocketConnectResult accept(
            websocket::Handler handler, std::string principal = {}) {
            return WebSocketConnectResult{
                .matched = true,
                .accepted = true,
                .rejection_status = 200,
                .rejection_body = {},
                .handler = std::move(handler),
                .principal = std::move(principal),
            };
        }

        [[nodiscard]] static WebSocketConnectResult reject(
            int status = 403, std::string body = "Forbidden") {
            return WebSocketConnectResult{
                .matched = true,
                .accepted = false,
                .rejection_status = status,
                .rejection_body = std::move(body),
                .handler = std::nullopt,
                .principal = {},
            };
        }
    };

    using WebSocketConnectHandler =
        std::function<WebSocketConnectResult(http3::Request&)>;

    struct Http2Stream {
        int32_t stream_id;
        http3::Request request;
        http3::Response response;
        bool request_complete = false;
        bool extended_websocket_connect = false;
        bool websocket_active = false;
        std::optional<websocket::Connection> websocket;
        std::shared_ptr<websocket::TransportBackpressure> websocket_backpressure;

        // HTTP/2 DATA may be flow-controlled.  The nghttp2 provider drains
        // this queue incrementally and leaves the CONNECT stream open after
        // each payload batch.
        std::deque<std::vector<std::uint8_t>> websocket_outbound;
        std::size_t websocket_outbound_offset = 0;
        bool websocket_data_submission_active = false;
    };

    explicit Http2Session(RequestHandler handler,
                          WebSocketConnectHandler websocket_handler = {},
                          websocket::Wakeup websocket_wakeup = {});
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

    /// Drain SessionHandle sends after the owning TCP shard is woken.
    std::vector<std::uint8_t> drain_websocket_outbound(
        std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)> encrypt_callback);

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
    void handle_websocket_connect(Http2Stream& stream);
    void queue_websocket_outbound(Http2Stream& stream, std::vector<std::uint8_t> data);
    void submit_websocket_data(Http2Stream& stream);
    static nghttp2_ssize websocket_data_source_read_callback(
        nghttp2_session* session, int32_t stream_id, std::uint8_t* buf,
        std::size_t length, std::uint32_t* data_flags,
        nghttp2_data_source* source, void* user_data);
    std::vector<uint8_t> send_pending_frames(
        std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback);

    nghttp2_session* session_ = nullptr;
    RequestHandler handler_;
    WebSocketConnectHandler websocket_handler_;
    websocket::Wakeup websocket_wakeup_;
    std::unordered_map<int32_t, Http2Stream> streams_;
    bool keep_alive_ = true;
};

} // namespace novaboot::http2
