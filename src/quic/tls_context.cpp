#include "novaboot/quic/tls_context.h"

#include <stdexcept>
#include <format>
#include <cstring>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <spdlog/spdlog.h>

namespace novaboot::quic {

namespace {

std::string get_openssl_error() {
    unsigned long err = ERR_get_error();
    if (err == 0) return "unknown error";
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return buf;
}

int alpn_select_callback(SSL* /*ssl*/,
                         const unsigned char** out,
                         unsigned char* outlen,
                         const unsigned char* in,
                         unsigned int inlen,
                         void* arg) {
    auto* alpn = static_cast<std::string*>(arg);

    // Build the wire-format ALPN: length-prefixed
    // "h3" → \x02h3
    unsigned char alpn_wire[256];
    alpn_wire[0] = static_cast<unsigned char>(alpn->size());
    std::memcpy(alpn_wire + 1, alpn->data(), alpn->size());
    unsigned int alpn_wire_len = static_cast<unsigned int>(1 + alpn->size());

    if (SSL_select_next_proto(
            const_cast<unsigned char**>(out), outlen,
            alpn_wire, alpn_wire_len,
            in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    return SSL_TLSEXT_ERR_OK;
}

} // anonymous namespace

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

TlsContext::TlsContext(TlsContext&& other) noexcept
    : ctx_(other.ctx_), alpn_(std::move(other.alpn_)) {
    other.ctx_ = nullptr;
}

TlsContext& TlsContext::operator=(TlsContext&& other) noexcept {
    if (this != &other) {
        if (ctx_) SSL_CTX_free(ctx_);
        ctx_ = other.ctx_;
        alpn_ = std::move(other.alpn_);
        other.ctx_ = nullptr;
    }
    return *this;
}

TlsContext TlsContext::create(const Config& config) {
    TlsContext tls;
    tls.alpn_ = std::make_shared<std::string>(config.alpn);

    // Create SSL_CTX with TLS server method
    tls.ctx_ = SSL_CTX_new(TLS_server_method());
    if (!tls.ctx_) {
        throw std::runtime_error(
            std::format("SSL_CTX_new failed: {}", get_openssl_error()));
    }

    // Set minimum TLS version to 1.3 (required for QUIC)
    SSL_CTX_set_min_proto_version(tls.ctx_, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(tls.ctx_, TLS1_3_VERSION);

    // Load certificate
    if (SSL_CTX_use_certificate_chain_file(
            tls.ctx_, config.cert_file.c_str()) != 1) {
        throw std::runtime_error(
            std::format("Failed to load certificate from '{}': {}",
                        config.cert_file, get_openssl_error()));
    }

    // Load private key
    if (SSL_CTX_use_PrivateKey_file(
            tls.ctx_, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        throw std::runtime_error(
            std::format("Failed to load private key from '{}': {}",
                        config.key_file, get_openssl_error()));
    }

    // Verify key matches certificate
    if (SSL_CTX_check_private_key(tls.ctx_) != 1) {
        throw std::runtime_error("Private key does not match certificate");
    }

    // ALPN selection callback
    SSL_CTX_set_alpn_select_cb(tls.ctx_, alpn_select_callback, tls.alpn_.get());

    spdlog::info("TLS context initialized (OpenSSL {}, cert={})",
                 OpenSSL_version(OPENSSL_VERSION), config.cert_file);

    return tls;
}

TlsContext TlsContext::create_client(ClientConfig config) {
    TlsContext tls;
    tls.alpn_ = std::make_shared<std::string>(config.alpn);

    // Create SSL_CTX with TLS client method
    tls.ctx_ = SSL_CTX_new(TLS_client_method());
    if (!tls.ctx_) {
        throw std::runtime_error(
            std::format("SSL_CTX_new (client) failed: {}", get_openssl_error()));
    }

    // TLS 1.3 only (required for QUIC)
    SSL_CTX_set_min_proto_version(tls.ctx_, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(tls.ctx_, TLS1_3_VERSION);

    // Peer verification
    if (config.verify_peer) {
        SSL_CTX_set_verify(tls.ctx_, SSL_VERIFY_PEER, nullptr);
        if (!config.ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(
                    tls.ctx_, config.ca_file.c_str(), nullptr) != 1) {
                throw std::runtime_error(
                    std::format("Failed to load CA file '{}': {}",
                                config.ca_file, get_openssl_error()));
            }
        } else {
            // Use system default CA bundle
            SSL_CTX_set_default_verify_paths(tls.ctx_);
        }
    } else {
        SSL_CTX_set_verify(tls.ctx_, SSL_VERIFY_NONE, nullptr);
    }

    // ALPN — client-side uses SSL_CTX_set_alpn_protos (protos list in wire format)
    // Wire format: length-prefixed: \x02h3
    const std::string& alpn = config.alpn;
    std::vector<unsigned char> alpn_wire;
    alpn_wire.push_back(static_cast<unsigned char>(alpn.size()));
    alpn_wire.insert(alpn_wire.end(), alpn.begin(), alpn.end());
    if (SSL_CTX_set_alpn_protos(tls.ctx_,
                                alpn_wire.data(),
                                static_cast<unsigned int>(alpn_wire.size())) != 0) {
        throw std::runtime_error(
            std::format("SSL_CTX_set_alpn_protos failed: {}", get_openssl_error()));
    }

    spdlog::info("TLS client context initialized (OpenSSL {}, verify_peer={})",
                 OpenSSL_version(OPENSSL_VERSION), config.verify_peer);

    return tls;
}

} // namespace novaboot::quic
