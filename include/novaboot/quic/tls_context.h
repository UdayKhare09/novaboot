#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <openssl/ssl.h>

namespace novaboot::quic {

/// Shared TLS context for the server.
///
/// Manages the SSL_CTX object, certificate/key loading, and ALPN
/// configuration. One TlsContext per server (shared read-only across shards).
///
/// Thread-safety: READ-ONLY after construction. Safe to share across threads.
class TlsContext {
public:
    struct Config {
        std::string cert_file;       ///< Path to PEM certificate file
        std::string key_file;        ///< Path to PEM private key file
        std::string alpn = "h3";     ///< ALPN protocol (always "h3")
    };

    ~TlsContext();

    // Non-copyable, movable
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    TlsContext(TlsContext&&) noexcept;
    TlsContext& operator=(TlsContext&&) noexcept;

    /// Create a TLS context with the given configuration
    static TlsContext create(const Config& config);

    /// Get the raw SSL_CTX pointer (for creating SSL sessions)
    [[nodiscard]] SSL_CTX* native_handle() const noexcept { return ctx_; }

    /// Get the configured ALPN protocol
    [[nodiscard]] const std::string& alpn() const noexcept { return alpn_; }

private:
    TlsContext() = default;

    SSL_CTX*    ctx_  = nullptr;
    std::string alpn_;
};

} // namespace novaboot::quic
