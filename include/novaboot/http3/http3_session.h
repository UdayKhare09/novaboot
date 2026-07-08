#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>

#include "novaboot/http3/http3_stream.h"

namespace novaboot::http3 {

/// Callback invoked when a complete HTTP request is ready for routing.
using RequestHandler =
    std::function<void(Http3Stream& stream)>;

/// HTTP/3 session wrapping nghttp3_conn.
///
/// Sits on top of a QUIC connection and handles HTTP/3 framing:
///   - QPACK header compression/decompression
///   - Request header/body reception
///   - Response submission
///   - Control streams and settings
///
/// Thread-safety: NOT thread-safe. Owned by a single QuicConnection.
class Http3Session {
public:
    ~Http3Session();

    // Non-copyable, non-movable
    Http3Session(const Http3Session&) = delete;
    Http3Session& operator=(const Http3Session&) = delete;

    /// Create an HTTP/3 session for a QUIC connection.
    /// The handler will be called when a request is ready for routing.
    static std::unique_ptr<Http3Session> create(
        ngtcp2_conn* quic_conn,
        RequestHandler handler);

    /// Feed stream data from ngtcp2 into nghttp3
    int on_stream_data(int64_t stream_id, const uint8_t* data,
                       size_t datalen, bool fin);

    /// Notify nghttp3 that a stream has been closed
    int on_stream_close(int64_t stream_id, uint64_t app_error_code);

    /// Notify nghttp3 that stream data has been acked by the peer
    int on_acked_stream_data(int64_t stream_id, uint64_t offset,
                             uint64_t datalen);

    /// Notify that the send window has been extended
    int on_extend_max_stream_data(int64_t stream_id);

    /// Get stream data to send (called by QuicConnection::on_write)
    /// Returns the number of vecs filled, or -1 if no data.
    nghttp3_ssize get_stream_data(int64_t* stream_id, int* fin,
                                  ngtcp2_vec* vec, size_t veccnt);

    /// Inform nghttp3 that data has been written to the QUIC stream
    void add_write_offset(int64_t stream_id, size_t datavcnt,
                          ngtcp2_vec* datav);

    /// Submit an HTTP response for a stream
    int submit_response(Http3Stream& stream);

    /// Get the nghttp3 connection object
    [[nodiscard]] nghttp3_conn* native_handle() const noexcept {
        return conn_;
    }

    /// Find a stream by ID
    [[nodiscard]] Http3Stream* find_stream(int64_t stream_id);

private:
    Http3Session() = default;

    // ─── nghttp3 callbacks ───────────────────────────────────────────
    static int on_recv_header(nghttp3_conn* conn, int64_t stream_id,
                              int32_t token,
                              nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                              uint8_t flags, void* conn_user_data,
                              void* stream_user_data);
    static int on_end_headers(nghttp3_conn* conn, int64_t stream_id,
                              int fin, void* conn_user_data,
                              void* stream_user_data);
    static int on_recv_data(nghttp3_conn* conn, int64_t stream_id,
                            const uint8_t* data, size_t datalen,
                            void* conn_user_data,
                            void* stream_user_data);
    static int on_end_stream(nghttp3_conn* conn, int64_t stream_id,
                             void* conn_user_data,
                             void* stream_user_data);
    static int on_acked_stream_data_cb(nghttp3_conn* conn, int64_t stream_id,
                                       uint64_t datalen,
                                       void* conn_user_data,
                                       void* stream_user_data);
    static int on_deferred_consume(nghttp3_conn* conn, int64_t stream_id,
                                   size_t nconsumed,
                                   void* conn_user_data,
                                   void* stream_user_data);
    static nghttp3_ssize on_read_data(nghttp3_conn* conn, int64_t stream_id,
                                      nghttp3_vec* vec, size_t veccnt,
                                      uint32_t* pflags,
                                      void* conn_user_data,
                                      void* stream_user_data);

    /// Find or create a stream
    Http3Stream& get_or_create_stream(int64_t stream_id);

    nghttp3_conn* conn_      = nullptr;
    ngtcp2_conn*  quic_conn_ = nullptr;

    /// Stream ID → Http3Stream
    std::unordered_map<int64_t, std::unique_ptr<Http3Stream>> streams_;

    /// Called when request is ready for routing
    RequestHandler handler_;
};

} // namespace novaboot::http3
