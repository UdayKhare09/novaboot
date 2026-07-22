#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <nghttp2/nghttp2.h>

#include "novaboot/core/server.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/testing/live_server.h"
#include "novaboot/websocket/websocket.h"

namespace {

std::string find_fixture(std::string_view name) {
    std::filesystem::path path{name};
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(path)) return path.string();
        path = "../" / path;
    }
    throw std::runtime_error("missing TLS test fixture: " + std::string(name));
}

class TlsSocket {
public:
    explicit TlsSocket(std::string_view alpn = {}, std::uint16_t port = 4438) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) throw std::runtime_error("could not allocate TLS client context");
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);

        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("could not create client socket");
        timeval timeout{.tv_sec = 2, .tv_usec = 0};
        (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
            ::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("could not connect TLS test client");
        }

        ssl_ = SSL_new(ctx_);
        if (!ssl_) throw std::runtime_error("could not allocate TLS client session");
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, "localhost");
        if (!alpn.empty()) {
            if (alpn.size() > 255) throw std::invalid_argument("ALPN value is too long");
            std::vector<unsigned char> protocols;
            protocols.reserve(alpn.size() + 1);
            protocols.push_back(static_cast<unsigned char>(alpn.size()));
            protocols.insert(protocols.end(), alpn.begin(), alpn.end());
            if (SSL_set_alpn_protos(ssl_, protocols.data(),
                                    static_cast<unsigned int>(protocols.size())) != 0) {
                throw std::runtime_error("could not set client ALPN");
            }
        }
        if (SSL_connect(ssl_) != 1) {
            const auto error = ERR_get_error();
            char text[256] = {};
            ERR_error_string_n(error, text, sizeof(text));
            throw std::runtime_error("TLS client handshake failed: " + std::string(text));
        }
    }

    ~TlsSocket() {
        if (ssl_) SSL_free(ssl_);
        if (fd_ >= 0) ::close(fd_);
        if (ctx_) SSL_CTX_free(ctx_);
    }

    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    void write_all(std::string_view value) {
        write_bytes({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }

    void write_bytes(std::span<const std::uint8_t> value) {
        std::size_t offset = 0;
        while (offset < value.size()) {
            const auto written = SSL_write(ssl_, value.data() + offset,
                                           static_cast<int>(value.size() - offset));
            if (written <= 0) throw std::runtime_error("TLS client write failed");
            offset += static_cast<std::size_t>(written);
        }
    }

    std::vector<std::uint8_t> read_bytes() {
        std::array<std::uint8_t, 4096> buffer{};
        const auto read = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
        if (read <= 0) throw std::runtime_error("TLS client read timed out or failed");
        return {buffer.begin(), buffer.begin() + read};
    }

    [[nodiscard]] std::string selected_alpn() const {
        const unsigned char* value = nullptr;
        unsigned int size = 0;
        SSL_get0_alpn_selected(ssl_, &value, &size);
        return {reinterpret_cast<const char*>(value), size};
    }

    std::string read_headers() {
        while (received_.find("\r\n\r\n") == std::string::npos) {
            read_more();
        }
        const auto end = received_.find("\r\n\r\n") + 4;
        auto headers = received_.substr(0, end);
        received_.erase(0, end);
        return headers;
    }

    std::string read_text_frame() {
        while (received_.size() < 2) read_more();
        const auto first = static_cast<unsigned char>(received_[0]);
        const auto second = static_cast<unsigned char>(received_[1]);
        if ((first & 0x0fU) != 0x1U || (second & 0x80U) != 0U) {
            throw std::runtime_error("invalid server WebSocket text frame");
        }
        std::size_t header_size = 2;
        std::size_t payload_size = second & 0x7fU;
        if (payload_size == 126) {
            while (received_.size() < 4) read_more();
            payload_size = (static_cast<unsigned char>(received_[2]) << 8U) |
                           static_cast<unsigned char>(received_[3]);
            header_size = 4;
        }
        while (received_.size() < header_size + payload_size) read_more();
        auto payload = received_.substr(header_size, payload_size);
        received_.erase(0, header_size + payload_size);
        return payload;
    }

private:
    void read_more() {
        const auto bytes = read_bytes();
        received_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string received_;
};

class H2LivePeer {
public:
    H2LivePeer() {
        nghttp2_session_callbacks* callbacks = nullptr;
        if (nghttp2_session_callbacks_new(&callbacks) != 0) {
            throw std::runtime_error("could not create HTTP/2 test client");
        }
        nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
        if (nghttp2_session_client_new2(&session_, callbacks, this, nullptr) != 0) {
            nghttp2_session_callbacks_del(callbacks);
            throw std::runtime_error("could not configure HTTP/2 test client");
        }
        nghttp2_session_callbacks_del(callbacks);
    }

    ~H2LivePeer() {
        if (session_) nghttp2_session_del(session_);
    }

    H2LivePeer(const H2LivePeer&) = delete;
    H2LivePeer& operator=(const H2LivePeer&) = delete;

    void submit_connect_settings() {
        const nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1},
        };
        if (nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, 1) != 0) {
            throw std::runtime_error("could not submit HTTP/2 CONNECT setting");
        }
    }

    void submit_websocket_connect(std::string_view cookie, std::string_view authority) {
        const auto header = [](std::string_view name, std::string_view value) {
            return nghttp2_nv{
                .name = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(name.data())),
                .value = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(value.data())),
                .namelen = name.size(),
                .valuelen = value.size(),
                .flags = NGHTTP2_NV_FLAG_NONE,
            };
        };
        const std::array headers{
            header(":method", "CONNECT"),
            header(":protocol", "websocket"),
            header(":scheme", "https"),
            header(":authority", authority),
            header(":path", "/ws/private"),
            header("cookie", cookie),
        };
        if (nghttp2_submit_headers(session_, NGHTTP2_FLAG_NONE, -1, nullptr,
                                   headers.data(), headers.size(), nullptr) < 0) {
            throw std::runtime_error("could not submit HTTP/2 WebSocket CONNECT");
        }
    }

    std::vector<std::uint8_t> take_outbound() {
        std::vector<std::uint8_t> output;
        const std::uint8_t* data = nullptr;
        while (const auto size = nghttp2_session_mem_send2(session_, &data)) {
            if (size < 0) throw std::runtime_error("could not serialize HTTP/2 frames");
            output.insert(output.end(), data, data + size);
        }
        return output;
    }

    void receive(std::span<const std::uint8_t> bytes) {
        if (nghttp2_session_mem_recv2(session_, bytes.data(), bytes.size()) < 0) {
            throw std::runtime_error("could not parse HTTP/2 server frames");
        }
    }

    [[nodiscard]] const std::string& status() const noexcept { return status_; }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }

private:
    static int on_header(nghttp2_session*, const nghttp2_frame* frame,
                         const std::uint8_t* name, std::size_t name_length,
                         const std::uint8_t* value, std::size_t value_length,
                         std::uint8_t, void* user_data) {
        auto* self = static_cast<H2LivePeer*>(user_data);
        if (frame->hd.stream_id == 1 &&
            std::string_view(reinterpret_cast<const char*>(name), name_length) == ":status") {
            self->status_.assign(reinterpret_cast<const char*>(value), value_length);
        }
        return 0;
    }

    static int on_data(nghttp2_session*, std::uint8_t, int32_t stream_id,
                       const std::uint8_t* data, std::size_t size, void* user_data) {
        auto* self = static_cast<H2LivePeer*>(user_data);
        if (stream_id == 1) self->data_.insert(self->data_.end(), data, data + size);
        return 0;
    }

    nghttp2_session* session_ = nullptr;
    std::string status_;
    std::vector<std::uint8_t> data_;
};

void flush(TlsSocket& socket, H2LivePeer& peer) {
    const auto bytes = peer.take_outbound();
    if (!bytes.empty()) socket.write_bytes(bytes);
}

} // namespace

TEST(LiveWebSocketTest, AcceptsCookieJwtOverRealTlsHttp11Upgrade) {
    using namespace novaboot;
    constexpr std::string_view secret = "live-websocket-test-secret";

    middleware::JwtMiddleware::Config jwt_config;
    jwt_config.allowed_algorithms = {middleware::JwtAlgorithm::HS256};
    jwt_config.hmac_secret = std::string(secret);
    jwt_config.jwt_cookie_name = "nova_access";
    middleware::JwtMiddleware jwt(jwt_config);

    middleware::JwtIssuer issuer({
        .algorithm = middleware::JwtAlgorithm::HS256,
        .hmac_secret = std::string(secret),
        .rsa_private_key_pem = {},
        .key_id = {},
        .include_issued_at = false,
    });
    middleware::JwtTokenBuilder claims;
    claims.subject("browser-user").expires_in(std::chrono::hours{1});
    const auto token = issuer.issue(claims);
    ASSERT_TRUE(token.has_value());

    auto app = Server::create()
        .bind("127.0.0.1", 4438)
        .tls(find_fixture("cert.pem"), find_fixture("key.pem"))
        .workers(1)
        .build();
    app->websocket("/ws/private", websocket::Handler{
        .on_open = [](websocket::Session& session) {
            EXPECT_EQ(session.principal(), "browser-user");
            EXPECT_TRUE(session.send_text("welcome " + std::string(session.principal())));
        },
        .authorize = jwt.websocket_authorizer(),
    });
    novaboot::testing::LiveServer live(std::move(app));

    TlsSocket socket;
    socket.write_all(
        "GET /ws/private HTTP/1.1\r\n"
        "Host: localhost:4438\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Cookie: nova_access=" + *token + "\r\n\r\n");

    const auto headers = socket.read_headers();
    EXPECT_TRUE(headers.starts_with("HTTP/1.1 101"));
    EXPECT_NE(headers.find("Sec-WebSocket-Accept:"), std::string::npos);
    EXPECT_EQ(socket.read_text_frame(), "welcome browser-user");
}

TEST(LiveWebSocketTest, AcceptsCookieJwtOverRealTlsHttp2ExtendedConnect) {
    using namespace novaboot;
    constexpr std::string_view secret = "live-websocket-h2-test-secret";

    middleware::JwtMiddleware::Config jwt_config;
    jwt_config.allowed_algorithms = {middleware::JwtAlgorithm::HS256};
    jwt_config.hmac_secret = std::string(secret);
    jwt_config.jwt_cookie_name = "nova_access";
    middleware::JwtMiddleware jwt(jwt_config);

    middleware::JwtIssuer issuer({
        .algorithm = middleware::JwtAlgorithm::HS256,
        .hmac_secret = std::string(secret),
        .rsa_private_key_pem = {},
        .key_id = {},
        .include_issued_at = false,
    });
    middleware::JwtTokenBuilder claims;
    claims.subject("h2-browser-user").expires_in(std::chrono::hours{1});
    const auto token = issuer.issue(claims);
    ASSERT_TRUE(token.has_value());

    auto app = Server::create()
        .bind("127.0.0.1", 4439)
        .tls(find_fixture("cert.pem"), find_fixture("key.pem"))
        .workers(1)
        .build();
    app->websocket("/ws/private", websocket::Handler{
        .on_open = [](websocket::Session& session) {
            EXPECT_EQ(session.principal(), "h2-browser-user");
            EXPECT_TRUE(session.send_text("welcome " + std::string(session.principal())));
        },
        .authorize = jwt.websocket_authorizer(),
    });
    novaboot::testing::LiveServer live(std::move(app));

    TlsSocket socket{"h2", 4439};
    ASSERT_EQ(socket.selected_alpn(), "h2");
    H2LivePeer peer;
    peer.submit_connect_settings();
    flush(socket, peer);
    peer.receive(socket.read_bytes());
    flush(socket, peer); // ACK server settings before opening the CONNECT stream.

    peer.submit_websocket_connect("nova_access=" + *token, "localhost:4439");
    flush(socket, peer);
    for (int i = 0; i < 4 && (peer.status() != "200" || peer.data().empty()); ++i) {
        peer.receive(socket.read_bytes());
        flush(socket, peer);
    }

    EXPECT_EQ(peer.status(), "200");
    ASSERT_GE(peer.data().size(), 2U);
    EXPECT_EQ(peer.data()[0], 0x81U);
    EXPECT_EQ(peer.data()[1], 23U);
    EXPECT_EQ(std::string(peer.data().begin() + 2, peer.data().end()),
              "welcome h2-browser-user");
}
