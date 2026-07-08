#pragma once

#include <cstdint>

#include <openssl/ssl.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

namespace novaboot::quic {

class TlsContext;

/// Per-connection TLS session.
///
/// Wraps an SSL object and the ngtcp2_crypto_ossl_ctx needed to bridge
/// OpenSSL's QUIC TLS API with ngtcp2.
///
/// Thread-safety: NOT thread-safe. Owned by a single QuicConnection.
class TlsSession {
public:
    TlsSession() = default;
    ~TlsSession();

    // Non-copyable, movable
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    TlsSession(TlsSession&& other) noexcept;
    TlsSession& operator=(TlsSession&& other) noexcept;

    /// Create a TLS session for a server-side QUIC connection.
    /// The conn_ref must outlive this session (it points back to the
    /// QuicConnection for ngtcp2 crypto callback routing).
    static TlsSession create_server(const TlsContext& tls_ctx,
                                    ngtcp2_crypto_conn_ref* conn_ref);

    /// Get the raw SSL pointer
    [[nodiscard]] SSL* native_handle() const noexcept { return ssl_; }

    /// Get the ngtcp2 crypto context
    [[nodiscard]] ngtcp2_crypto_ossl_ctx* crypto_ctx() const noexcept {
        return crypto_ctx_;
    }

private:
    SSL*                    ssl_        = nullptr;
    ngtcp2_crypto_ossl_ctx* crypto_ctx_ = nullptr;
};

} // namespace novaboot::quic
