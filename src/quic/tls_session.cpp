#include "novaboot/quic/tls_session.h"
#include "novaboot/quic/tls_context.h"

#include <format>
#include <stdexcept>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <spdlog/spdlog.h>

namespace novaboot::quic {

TlsSession::~TlsSession() {
    if (ssl_) {
        // Clear app data before freeing SSL to prevent dangling pointer access
        SSL_set_app_data(ssl_, nullptr);
        SSL_free(ssl_);
    }
    if (crypto_ctx_) {
        ngtcp2_crypto_ossl_ctx_del(crypto_ctx_);
    }
}

TlsSession::TlsSession(TlsSession&& other) noexcept
    : ssl_(other.ssl_), crypto_ctx_(other.crypto_ctx_) {
    other.ssl_        = nullptr;
    other.crypto_ctx_ = nullptr;
}

TlsSession& TlsSession::operator=(TlsSession&& other) noexcept {
    if (this != &other) {
        if (ssl_) {
            SSL_set_app_data(ssl_, nullptr);
            SSL_free(ssl_);
        }
        if (crypto_ctx_) {
            ngtcp2_crypto_ossl_ctx_del(crypto_ctx_);
        }

        ssl_              = other.ssl_;
        crypto_ctx_       = other.crypto_ctx_;
        other.ssl_        = nullptr;
        other.crypto_ctx_ = nullptr;
    }
    return *this;
}

TlsSession TlsSession::create_server(const TlsContext& tls_ctx,
                                      ngtcp2_crypto_conn_ref* conn_ref) {
    TlsSession session;

    // Create SSL object from context
    session.ssl_ = SSL_new(tls_ctx.native_handle());
    if (!session.ssl_) {
        throw std::runtime_error("SSL_new failed");
    }

    // Create ngtcp2 crypto context (bridges ngtcp2 ↔ OpenSSL)
    int rv = ngtcp2_crypto_ossl_ctx_new(&session.crypto_ctx_, session.ssl_);
    if (rv != 0) {
        throw std::runtime_error(
            std::format("ngtcp2_crypto_ossl_ctx_new failed: {}", rv));
    }

    // Configure SSL for server-side QUIC
    rv = ngtcp2_crypto_ossl_configure_server_session(session.ssl_);
    if (rv != 0) {
        throw std::runtime_error(
            "ngtcp2_crypto_ossl_configure_server_session failed");
    }

    // Set the conn_ref as app data — ngtcp2 crypto callbacks use this
    // to find the ngtcp2_conn from the SSL object
    SSL_set_app_data(session.ssl_, conn_ref);

    // Set SSL to accept (server mode)
    SSL_set_accept_state(session.ssl_);

    return session;
}

} // namespace novaboot::quic
