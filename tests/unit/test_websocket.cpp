#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include "novaboot/http1/http1_session.h"
#include "novaboot/http2/http2_session.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/websocket/websocket.h"

namespace {

std::vector<std::uint8_t> client_frame(
    novaboot::websocket::Opcode opcode, std::string_view text, bool fin = true) {
    const std::array<std::uint8_t, 4> mask{0x11, 0x22, 0x33, 0x44};
    std::vector<std::uint8_t> frame;
    frame.push_back(static_cast<std::uint8_t>((fin ? 0x80U : 0U) |
                                              static_cast<std::uint8_t>(opcode)));
    frame.push_back(static_cast<std::uint8_t>(0x80U | text.size()));
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < text.size(); ++i) {
        frame.push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[i]) ^ mask[i % mask.size()]));
    }
    return frame;
}

novaboot::http3::Request valid_upgrade_request() {
    novaboot::http3::Request request;
    request.set_method("GET");
    request.set_path("/ws/chat");
    request.headers().add("Upgrade", "websocket");
    request.headers().add("Connection", "keep-alive, Upgrade");
    request.headers().add("Sec-WebSocket-Version", "13");
    request.headers().add("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    return request;
}

struct H2TestPeer {
    nghttp2_session* session = nullptr;
    std::string status;
    std::vector<std::uint8_t> received_data;
    bool server_advertised_extended_connect = false;

    H2TestPeer() {
        nghttp2_session_callbacks* callbacks = nullptr;
        if (nghttp2_session_callbacks_new(&callbacks) != 0) {
            throw std::runtime_error("could not allocate nghttp2 callbacks");
        }
        nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv);
        if (nghttp2_session_client_new2(&session, callbacks, this, nullptr) != 0) {
            nghttp2_session_callbacks_del(callbacks);
            throw std::runtime_error("could not create nghttp2 client session");
        }
        nghttp2_session_callbacks_del(callbacks);
    }

    ~H2TestPeer() {
        if (session) nghttp2_session_del(session);
    }

    H2TestPeer(const H2TestPeer&) = delete;
    H2TestPeer& operator=(const H2TestPeer&) = delete;

    void submit_connect_settings() {
        const nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1},
        };
        ASSERT_EQ(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, 1), 0);
    }

    [[nodiscard]] int32_t submit_websocket_connect() {
        auto nv = [](std::string_view name, std::string_view value) {
            nghttp2_nv header{};
            header.name = const_cast<std::uint8_t*>(
                reinterpret_cast<const std::uint8_t*>(name.data()));
            header.value = const_cast<std::uint8_t*>(
                reinterpret_cast<const std::uint8_t*>(value.data()));
            header.namelen = name.size();
            header.valuelen = value.size();
            header.flags = NGHTTP2_NV_FLAG_NONE;
            return header;
        };
        const std::array headers{
            nv(":method", "CONNECT"),
            nv(":protocol", "websocket"),
            nv(":scheme", "https"),
            nv(":authority", "example.test"),
            nv(":path", "/ws/h2"),
        };
        return nghttp2_submit_headers(session, NGHTTP2_FLAG_NONE, -1, nullptr,
                                      headers.data(), headers.size(), nullptr);
    }

    [[nodiscard]] std::vector<std::uint8_t> take_outbound() {
        std::vector<std::uint8_t> output;
        const std::uint8_t* data = nullptr;
        while (const auto size = nghttp2_session_mem_send2(session, &data)) {
            if (size < 0) return {};
            output.insert(output.end(), data, data + size);
        }
        return output;
    }

    void receive(std::span<const std::uint8_t> data) {
        ASSERT_GE(nghttp2_session_mem_recv2(session, data.data(), data.size()), 0);
    }

    static int on_header(nghttp2_session*, const nghttp2_frame* frame,
                         const std::uint8_t* name, std::size_t namelen,
                         const std::uint8_t* value, std::size_t valuelen,
                         std::uint8_t, void* user_data) {
        auto* peer = static_cast<H2TestPeer*>(user_data);
        if (frame->hd.stream_id == 1 && std::string_view(
                reinterpret_cast<const char*>(name), namelen) == ":status") {
            peer->status.assign(reinterpret_cast<const char*>(value), valuelen);
        }
        return 0;
    }

    static int on_data(nghttp2_session*, std::uint8_t, int32_t,
                       const std::uint8_t* data, std::size_t len, void* user_data) {
        auto* peer = static_cast<H2TestPeer*>(user_data);
        peer->received_data.insert(peer->received_data.end(), data, data + len);
        return 0;
    }

    static int on_frame_recv(nghttp2_session*, const nghttp2_frame* frame,
                             void* user_data) {
        auto* peer = static_cast<H2TestPeer*>(user_data);
        if (frame->hd.type != NGHTTP2_SETTINGS) return 0;
        for (std::size_t index = 0; index < frame->settings.niv; ++index) {
            const auto& entry = frame->settings.iv[index];
            if (entry.settings_id == NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL &&
                entry.value == 1U) {
                peer->server_advertised_extended_connect = true;
            }
        }
        return 0;
    }
};

struct H2TestDataSource {
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
};

nghttp2_ssize h2_test_data_read(nghttp2_session*, int32_t, std::uint8_t* buffer,
                                std::size_t length, std::uint32_t* flags,
                                nghttp2_data_source* source, void*) {
    auto* data = static_cast<H2TestDataSource*>(source->ptr);
    const auto count = std::min(length, data->bytes.size() - data->offset);
    std::copy_n(data->bytes.data() + data->offset, count, buffer);
    data->offset += count;
    if (data->offset == data->bytes.size()) {
        *flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
    }
    return static_cast<nghttp2_ssize>(count);
}

} // namespace

TEST(WebSocketHandshakeTest, ValidatesRfc6455HandshakeAndCalculatesAcceptKey) {
    const auto accept = novaboot::websocket::validate_upgrade_request(valid_upgrade_request());

    ASSERT_TRUE(accept.has_value());
    EXPECT_EQ(*accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketHandshakeTest, RejectsMissingConnectionUpgradeToken) {
    auto request = valid_upgrade_request();
    request.headers().set("Connection", "keep-alive");

    const auto result = novaboot::websocket::validate_upgrade_request(request);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().close_code, 1002);
}

TEST(WebSocketConnectionTest, DeliversMaskedTextFramesAndLetsHandlerReply) {
    std::string received;
    bool opened = false;
    novaboot::websocket::Connection connection({
        .on_open = [&](novaboot::websocket::Session&) { opened = true; },
        .on_message = [&](novaboot::websocket::Session& session,
                          const novaboot::websocket::Message& message) {
            received = std::string(message.text());
            session.send_text("ack");
        },
        .on_close = {},
        .authorize = {},
    });

    const auto result = connection.feed(client_frame(novaboot::websocket::Opcode::Text, "hello"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(opened);
    EXPECT_EQ(received, "hello");
    const auto outbound = connection.take_outbound();
    ASSERT_EQ(outbound.size(), 5U);
    EXPECT_EQ(outbound[0], 0x81U);
    EXPECT_EQ(outbound[1], 3U);
    EXPECT_EQ(std::string(outbound.begin() + 2, outbound.end()), "ack");
}

TEST(WebSocketConnectionTest, ReassemblesFragmentedMessagesAndAnswersPings) {
    std::string received;
    novaboot::websocket::Connection connection({
        .on_open = {},
        .on_message = [&](novaboot::websocket::Session&,
                          const novaboot::websocket::Message& message) {
            received = std::string(message.text());
        },
        .on_close = {},
        .authorize = {},
    });

    ASSERT_TRUE(connection.feed(client_frame(novaboot::websocket::Opcode::Text, "hel", false)).has_value());
    ASSERT_TRUE(connection.feed(client_frame(novaboot::websocket::Opcode::Ping, "x")).has_value());
    ASSERT_TRUE(connection.feed(client_frame(novaboot::websocket::Opcode::Continuation, "lo")).has_value());

    EXPECT_EQ(received, "hello");
    const auto outbound = connection.take_outbound();
    ASSERT_EQ(outbound.size(), 3U);
    EXPECT_EQ(outbound[0], 0x8AU);
    EXPECT_EQ(outbound[1], 1U);
    EXPECT_EQ(outbound[2], static_cast<std::uint8_t>('x'));
}

TEST(WebSocketConnectionTest, RejectsUnmaskedClientFramesWithProtocolClose) {
    novaboot::websocket::Connection connection({});
    const std::array<std::uint8_t, 4> unmasked{0x81, 0x02, 'n', 'o'};

    const auto result = connection.feed(unmasked);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().close_code, 1002);
    EXPECT_TRUE(connection.closed());
    const auto outbound = connection.take_outbound();
    ASSERT_GE(outbound.size(), 4U);
    EXPECT_EQ(outbound[0], 0x88U);
}

TEST(WebSocketConnectionTest, BoundsSlowClientOutboundQueueWithTryAgainLaterClose) {
    bool accepted = true;
    novaboot::websocket::Connection connection({
        .on_open = {},
        .on_message = [&](novaboot::websocket::Session& session,
                          const novaboot::websocket::Message&) {
            accepted = session.send_text("this message exceeds the endpoint queue");
        },
        .on_close = {},
        .authorize = {},
        .limits = {
            .max_message_bytes = 1024U,
            .max_pending_send_bytes = 8U,
        },
    });

    ASSERT_TRUE(connection.feed(client_frame(novaboot::websocket::Opcode::Text, "go")).has_value());
    EXPECT_FALSE(accepted);
    const auto outbound = connection.take_outbound();
    ASSERT_GE(outbound.size(), 4U);
    EXPECT_EQ(outbound[0], 0x88U);
    EXPECT_EQ(outbound[1] & 0x7fU, 31U);
    EXPECT_EQ(outbound[2], 0x03U);
    EXPECT_EQ(outbound[3], 0xf5U); // 1013, Try Again Later
}

TEST(WebSocketConnectionTest, PropagatesTransportFlowControlToSendResult) {
    bool accepted = true;
    auto transport_budget =
        std::make_shared<novaboot::websocket::TransportBackpressure>(8U);
    novaboot::websocket::Connection connection({
        .on_open = {},
        .on_message = [&](novaboot::websocket::Session& session,
                          const novaboot::websocket::Message&) {
            // The WebSocket frame is 2-byte header + 7-byte payload.  The
            // connection-local queue has room, but a flow-controlled HTTP/2
            // transport does not.
            accepted = session.send_text("blocked");
        },
        .on_close = {},
        .authorize = {},
        .limits = {
            .max_message_bytes = 1024U,
            .max_pending_send_bytes = 128U,
        },
    }, {}, {}, transport_budget);

    ASSERT_TRUE(connection.feed(client_frame(novaboot::websocket::Opcode::Text, "go")).has_value());
    EXPECT_FALSE(accepted);
    const auto outbound = connection.take_outbound();
    ASSERT_GE(outbound.size(), 4U);
    EXPECT_EQ(outbound[0], 0x88U);
    EXPECT_EQ(outbound[2], 0x03U);
    EXPECT_EQ(outbound[3], 0xf5U); // 1013, Try Again Later
}

TEST(WebSocketConnectionTest, RegistryQueuesBroadcastsForTheOwningConnection) {
    novaboot::websocket::SessionRegistry registry;
    novaboot::websocket::SessionHandle handle;
    int wakeups = 0;

    {
        novaboot::websocket::Connection connection({
            .on_open = [&](novaboot::websocket::Session& session) {
                handle = session.handle();
                registry.add(handle);
            },
            .on_message = {},
            .on_close = {},
            .authorize = {},
        }, {}, [&] { ++wakeups; });

        ASSERT_TRUE(handle.active());
        EXPECT_EQ(registry.broadcast_text("notice"), 1U);
        EXPECT_EQ(wakeups, 1);
        const auto outbound = connection.drain_external_outbound();
        ASSERT_EQ(outbound.size(), 8U);
        EXPECT_EQ(outbound[0], 0x81U);
        EXPECT_EQ(outbound[1], 6U);
        EXPECT_EQ(std::string(outbound.begin() + 2, outbound.end()), "notice");
    }

    EXPECT_FALSE(handle.active());
    EXPECT_EQ(registry.broadcast_text("ignored"), 0U);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(Http1WebSocketUpgradeTest, SwitchesConnectionModeAndPreservesTheHandler) {
    bool opened = false;
    std::string principal;
    novaboot::http1::Http1Session session(
        [](auto&, auto&) {},
        [&](novaboot::http3::Request& request)
            -> novaboot::http1::Http1Session::UpgradeResult {
            if (request.path() != "/ws/chat") return {};
            return novaboot::http1::Http1Session::UpgradeResult::accept(
                novaboot::websocket::Handler{
                    .on_open = [&](novaboot::websocket::Session& socket) {
                        opened = true;
                        principal = socket.principal();
                    },
                    .on_message = {},
                    .on_close = {},
                    .authorize = {},
                },
                "user-42");
        });

    const std::string request =
        "GET /ws/chat HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    const auto result = session.feed_data(
        {request.begin(), request.end()},
        [](const std::vector<std::uint8_t>& bytes) { return bytes; });

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(session.upgraded());
    const std::string response(result->begin(), result->end());
    EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);

    auto handler = session.take_upgrade_handler();
    ASSERT_TRUE(handler.has_value());
    novaboot::websocket::Connection connection(
        std::move(handler->handler), std::move(handler->principal));
    EXPECT_TRUE(opened);
    EXPECT_EQ(principal, "user-42");
}

TEST(Http2WebSocketConnectTest, RunsTheSameHandlerOverRfc8441ExtendedConnect) {
    bool opened = false;
    std::string received;
    novaboot::http2::Http2Session server(
        [](auto&, auto&) {},
        [&](novaboot::http3::Request& request) {
            if (request.path() != "/ws/h2") {
                return novaboot::http2::Http2Session::WebSocketConnectResult{};
            }
            return novaboot::http2::Http2Session::WebSocketConnectResult::accept(
                novaboot::websocket::Handler{
                    .on_open = [&](novaboot::websocket::Session& socket) {
                        opened = true;
                        socket.send_text("ready");
                    },
                    .on_message = [&](novaboot::websocket::Session&,
                                      const novaboot::websocket::Message& message) {
                        received = std::string(message.text());
                    },
                    .on_close = {},
                    .authorize = {},
                },
                "h2-user");
        });
    const auto passthrough = [](const std::vector<std::uint8_t>& bytes) { return bytes; };
    H2TestPeer peer;

    peer.submit_connect_settings();
    auto server_reply = server.feed_data(peer.take_outbound(), passthrough);
    ASSERT_TRUE(server_reply.has_value());
    peer.receive(*server_reply);
    EXPECT_TRUE(peer.server_advertised_extended_connect);
    auto client_ack = peer.take_outbound();
    ASSERT_TRUE(server.feed_data(client_ack, passthrough).has_value());

    ASSERT_EQ(peer.submit_websocket_connect(), 1);
    server_reply = server.feed_data(peer.take_outbound(), passthrough);
    ASSERT_TRUE(server_reply.has_value());
    peer.receive(*server_reply);
    EXPECT_EQ(peer.status, "200");
    EXPECT_TRUE(opened);
    ASSERT_EQ(peer.received_data.size(), 7U);
    EXPECT_EQ(peer.received_data[0], 0x81U);
    EXPECT_EQ(peer.received_data[1], 5U);
    EXPECT_EQ(std::string(peer.received_data.begin() + 2, peer.received_data.end()), "ready");

    H2TestDataSource source{.bytes = client_frame(novaboot::websocket::Opcode::Text, "hello")};
    nghttp2_data_provider provider{};
    provider.source.ptr = &source;
    provider.read_callback = h2_test_data_read;
    ASSERT_EQ(nghttp2_submit_data(peer.session, NGHTTP2_FLAG_NONE, 1, &provider), 0);
    ASSERT_TRUE(server.feed_data(peer.take_outbound(), passthrough).has_value());
    EXPECT_EQ(received, "hello");
}

TEST(Http2WebSocketConnectTest, RejectsUnauthorizedExtendedConnectBeforeUpgrade) {
    const auto passthrough = [](const std::vector<std::uint8_t>& bytes) { return bytes; };
    novaboot::http2::Http2Session server(
        [](auto&, auto&) {},
        [](novaboot::http3::Request&) {
            return novaboot::http2::Http2Session::WebSocketConnectResult::reject(
                401, "Authentication required");
        });
    H2TestPeer peer;
    peer.submit_connect_settings();
    auto server_reply = server.feed_data(peer.take_outbound(), passthrough);
    ASSERT_TRUE(server_reply.has_value());
    peer.receive(*server_reply);
    ASSERT_TRUE(server.feed_data(peer.take_outbound(), passthrough).has_value());

    ASSERT_EQ(peer.submit_websocket_connect(), 1);
    server_reply = server.feed_data(peer.take_outbound(), passthrough);
    ASSERT_TRUE(server_reply.has_value());
    peer.receive(*server_reply);

    EXPECT_EQ(peer.status, "401");
    EXPECT_EQ(std::string(peer.received_data.begin(), peer.received_data.end()),
              "Authentication required");
}

TEST(Http1WebSocketUpgradeTest, RejectsUnauthorizedHandshakeBeforeSwitchingProtocols) {
    novaboot::http1::Http1Session session(
        [](auto&, auto&) {},
        [](novaboot::http3::Request&) {
            return novaboot::http1::Http1Session::UpgradeResult::reject(
                401, "Authentication required");
        });

    const std::string request =
        "GET /ws/private HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    const auto result = session.feed_data(
        {request.begin(), request.end()},
        [](const std::vector<std::uint8_t>& bytes) { return bytes; });

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(session.upgraded());
    const std::string response(result->begin(), result->end());
    EXPECT_NE(response.find("401 Unauthorized"), std::string::npos);
    EXPECT_NE(response.find("Authentication required"), std::string::npos);
}

TEST(Http1WebSocketUpgradeTest, AcceptsVerifiedJwtAndExposesItsSubjectToTheSession) {
    novaboot::middleware::JwtMiddleware::Config jwt_config;
    jwt_config.allowed_algorithms = {novaboot::middleware::JwtAlgorithm::HS256};
    jwt_config.hmac_secret = "websocket-test-secret";
    novaboot::middleware::JwtMiddleware jwt(jwt_config);
    const auto authorize = jwt.websocket_authorizer();

    novaboot::middleware::JwtIssuer issuer({
        .algorithm = novaboot::middleware::JwtAlgorithm::HS256,
        .hmac_secret = "websocket-test-secret",
        .rsa_private_key_pem = {},
        .key_id = {},
        .include_issued_at = false,
    });
    novaboot::middleware::JwtTokenBuilder claims;
    claims.subject("socket-user").expires_in(std::chrono::hours{1});
    const auto token = issuer.issue(claims);
    ASSERT_TRUE(token.has_value());

    std::string principal;
    novaboot::websocket::Handler endpoint{
        .on_open = [&](novaboot::websocket::Session& session) {
            principal = session.principal();
        },
    };
    novaboot::http1::Http1Session session(
        [](auto&, auto&) {},
        [authorize, endpoint](novaboot::http3::Request& request)
            mutable -> novaboot::http1::Http1Session::UpgradeResult {
            const auto decision = authorize(request);
            if (!decision.accepted) {
                return novaboot::http1::Http1Session::UpgradeResult::reject(
                    decision.rejection_status, decision.rejection_body);
            }
            return novaboot::http1::Http1Session::UpgradeResult::accept(
                std::move(endpoint), decision.principal);
        });

    const std::string request =
        "GET /ws/private HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Authorization: Bearer " + *token + "\r\n\r\n";
    const auto response = session.feed_data(
        {request.begin(), request.end()},
        [](const std::vector<std::uint8_t>& bytes) { return bytes; });

    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(session.upgraded());
    auto accepted = session.take_upgrade_handler();
    ASSERT_TRUE(accepted.has_value());
    novaboot::websocket::Connection connection(
        std::move(accepted->handler), std::move(accepted->principal));
    EXPECT_EQ(principal, "socket-user");
}
