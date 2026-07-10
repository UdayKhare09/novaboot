#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>

#include "novaboot/http3/client_response.h"
#include "novaboot/http3/header_map.h"

namespace novaboot::http3 {

/// Callback invoked when a complete HTTP response has been received.
using ResponseHandler = std::function<void(int64_t stream_id, ClientResponse)>;

/// Client-side HTTP/3 session wrapping nghttp3_conn (client mode).
///
/// Submits outgoing requests and receives incoming responses.
/// Thread-safety: NOT thread-safe. Owned by a single QuicClientConnection.
class Http3ClientSession {
public:
    ~Http3ClientSession();

    Http3ClientSession(const Http3ClientSession&) = delete;
    Http3ClientSession& operator=(const Http3ClientSession&) = delete;

    /// Create a client HTTP/3 session.
    /// @param quic_conn     The ngtcp2 connection owning this session
    /// @param resp_handler  Called when a response stream completes
    static std::unique_ptr<Http3ClientSession> create(
        ngtcp2_conn* quic_conn,
        ResponseHandler resp_handler);

    /// Submit an HTTP request on a new stream.
    /// @returns the stream_id on success, or -1 on failure.
    int64_t submit_request(std::string_view method,
                           std::string_view path,
                           std::string_view authority,
                           std::string_view body     = {},
                           const HeaderMap& headers  = {});

    // ─── Called by QuicClientConnection ─────────────────────────────
    int on_stream_data(int64_t stream_id, uint64_t offset,
                       const uint8_t* data, size_t datalen, bool fin);
    int on_stream_close(int64_t stream_id, uint64_t app_error_code);
    int on_acked_stream_data(int64_t stream_id, uint64_t offset,
                             uint64_t datalen);
    int on_extend_max_stream_data(int64_t stream_id);

    nghttp3_ssize get_stream_data(int64_t* stream_id, int* fin,
                                  ngtcp2_vec* vec, size_t veccnt);
    void add_write_offset(int64_t stream_id, size_t datalen);

    [[nodiscard]] nghttp3_conn* native_handle() const noexcept { return conn_; }

private:
    Http3ClientSession() = default;

    // ─── Per-stream state ────────────────────────────────────────────
    struct StreamState {
        ClientResponse response;
        bool           headers_done = false;
    };

    StreamState& get_or_create(int64_t stream_id);
    StreamState* find(int64_t stream_id);

    // ─── nghttp3 callbacks ───────────────────────────────────────────
    static int on_recv_header(nghttp3_conn*, int64_t stream_id, int32_t token,
                              nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                              uint8_t flags, void* conn_user_data,
                              void* stream_user_data);
    static int on_end_headers(nghttp3_conn*, int64_t stream_id, int fin,
                              void* conn_user_data, void* stream_user_data);
    static int on_recv_data(nghttp3_conn*, int64_t stream_id,
                            const uint8_t* data, size_t datalen,
                            void* conn_user_data, void* stream_user_data);
    static int on_end_stream(nghttp3_conn*, int64_t stream_id,
                             void* conn_user_data, void* stream_user_data);
    static int on_acked_stream_data_cb(nghttp3_conn*, int64_t stream_id,
                                       uint64_t datalen, void* conn_user_data,
                                       void* stream_user_data);
    static int on_deferred_consume(nghttp3_conn*, int64_t stream_id,
                                   size_t nconsumed, void* conn_user_data,
                                   void* stream_user_data);
    static nghttp3_ssize on_read_data(nghttp3_conn*, int64_t stream_id,
                                      nghttp3_vec* vec, size_t veccnt,
                                      uint32_t* pflags, void* conn_user_data,
                                      void* stream_user_data);

    nghttp3_conn* conn_      = nullptr;
    ngtcp2_conn*  quic_conn_ = nullptr;

    ResponseHandler handler_;

    /// Per-stream response state (stream_id → state)
    std::unordered_map<int64_t, StreamState> streams_;
    /// Tracks read offsets for duplicate detection
    std::unordered_map<int64_t, uint64_t> stream_read_offsets_;

    /// Per-stream request body (stream_id → body string)
    struct RequestBody {
        std::string data;
        size_t      provided = 0;
    };
    std::unordered_map<int64_t, RequestBody> request_bodies_;
};

} // namespace novaboot::http3
