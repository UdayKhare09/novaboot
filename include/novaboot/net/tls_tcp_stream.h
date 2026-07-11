#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <memory>
#include <string_view>
#include <vector>
#include <span>
#include <expected>
#include <functional>

namespace novaboot::net {

class TlsTcpStream {
public:
    enum class HandshakeStatus {
        Handshaking,
        Complete,
        Failed,
    };

    TlsTcpStream(int fd, SSL_CTX* ssl_ctx, bool is_server = true, const std::string& host = "");
    ~TlsTcpStream();

    TlsTcpStream(const TlsTcpStream&) = delete;
    TlsTcpStream& operator=(const TlsTcpStream&) = delete;
    TlsTcpStream(TlsTcpStream&& other) noexcept;
    TlsTcpStream& operator=(TlsTcpStream&& other) noexcept;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    struct TlsData {
        std::vector<uint8_t> decrypted_app_data;
        std::vector<uint8_t> handshake_send_data;
    };

    [[nodiscard]] HandshakeStatus handshake_status() const noexcept { return handshake_status_; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept;

    /// Feed raw encrypted data received from the network into OpenSSL.
    std::expected<TlsData, int> feed_network_data(std::span<const uint8_t> data);

    /// Encrypt application data and retrieve the raw encrypted bytes to be sent to the network.
    std::expected<std::vector<uint8_t>, int> encrypt_app_data(std::span<const uint8_t> data);

    /// Perform/resume TLS handshake.
    /// Returns the current handshake status or an error code.
    /// Fills out_network_data with any handshake packets to send to the network.
    std::expected<HandshakeStatus, int> do_handshake(std::vector<uint8_t>& out_network_data);

    /// Retrieve any pending encrypted bytes that SSL wants to send to the network.
    std::vector<uint8_t> get_pending_send_data();

private:
    int fd_ = -1;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;
    BIO* wbio_ = nullptr;
    HandshakeStatus handshake_status_ = HandshakeStatus::Handshaking;
};

} // namespace novaboot::net
