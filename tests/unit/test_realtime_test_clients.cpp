#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "novaboot/testing/stomp_test_client.h"
#include "novaboot/testing/websocket_test_client.h"

TEST(WebSocketTestClientTest, SendsMaskedFramesAndAssertsEndpointReplies) {
    novaboot::testing::WebSocketTestClient client({
        .on_message = [](novaboot::websocket::Session& session,
                         const novaboot::websocket::Message& message) {
            session.send_text("echo: " + std::string(message.text()));
        },
    });

    client.send_text("hello");
    EXPECT_NO_THROW(client.require_single_text("echo: hello"));
}

TEST(WebSocketTestClientTest, ExposesEndpointCloseCodes) {
    novaboot::testing::WebSocketTestClient client({
        .on_message = [](novaboot::websocket::Session& session,
                         const novaboot::websocket::Message&) {
            session.close(1008, "policy");
        },
    });

    client.send_text("forbidden");
    EXPECT_NO_THROW(client.require_close(1008));
    client.close(1008, "policy");
    EXPECT_TRUE(client.closed());
}

TEST(StompTestClientTest, ConnectsAndAssertsSubscriptionReceipt) {
    novaboot::messaging::stomp::SimpleBroker broker;
    novaboot::messaging::stomp::Endpoint endpoint(broker);
    novaboot::testing::StompTestClient client(endpoint, "alice");

    EXPECT_NO_THROW(client.connect());
    client.send({
        .command = "SUBSCRIBE",
        .headers = {
            {"id", "chat-1"},
            {"destination", "/topic/chat"},
            {"receipt", "subscribed"},
        },
        .body = {},
    });
    EXPECT_NO_THROW(client.require_receipt("subscribed"));
}

TEST(StompTestClientTest, ObservesNegotiatedServerHeartbeats) {
    novaboot::messaging::stomp::SimpleBroker broker;
    novaboot::messaging::stomp::Endpoint endpoint(broker, {
        .server_outgoing_heartbeat = std::chrono::milliseconds{20},
        .server_incoming_heartbeat = std::chrono::milliseconds{20},
    });
    novaboot::testing::StompTestClient client(endpoint);

    // The client advertises that it can receive server heartbeats.
    client.connect("20,0");
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    EXPECT_GE(client.take_heartbeats(), 1U);
}

TEST(StompRelayTest, ForwardsFramesAndReturnsBrokerFrames) {
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t address_size = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size), 0);

    std::string received;
    std::jthread broker([&] {
        const int peer = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer < 0) return;
        char buffer[4096];
        while (received.find("SEND\n") == std::string::npos) {
            const auto count = ::recv(peer, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            received.append(buffer, static_cast<std::size_t>(count));
        }
        constexpr char reply[] =
            "CONNECTED\nversion:1.2\n\n\0MESSAGE\nsubscription:chat\ndestination:/topic/chat\n\nhello\0";
        (void)::send(peer, reply, sizeof(reply) - 1U, MSG_NOSIGNAL);
        ::close(peer);
    });

    novaboot::messaging::stomp::RelayEndpoint relay({
        .host = "127.0.0.1",
        .port = ntohs(address.sin_port),
        .reconnect_delay = std::chrono::milliseconds{10},
    });
    novaboot::testing::WebSocketTestClient client(relay.websocket_handler());
    std::string connect = "CONNECT\naccept-version:1.2\n\n";
    connect.push_back('\0');
    std::string subscribe = "SUBSCRIBE\nid:chat\ndestination:/topic/chat\n\n";
    subscribe.push_back('\0');
    std::string send = "SEND\ndestination:/topic/chat\n\nhello";
    send.push_back('\0');
    client.send_text(connect);
    client.send_text(subscribe);
    client.send_text(send);

    std::vector<novaboot::testing::WebSocketTestFrame> frames;
    for (int attempt = 0; attempt < 50 && frames.size() < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        auto next = client.take_frames();
        frames.insert(frames.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
    }
    EXPECT_NE(received.find("CONNECT\n"), std::string::npos);
    EXPECT_NE(received.find("SUBSCRIBE\n"), std::string::npos);
    EXPECT_NE(received.find("SEND\n"), std::string::npos);
    ASSERT_GE(frames.size(), 2U);
    EXPECT_NE(frames[0].text().find("CONNECTED\n"), std::string_view::npos);
    EXPECT_NE(frames[1].text().find("MESSAGE\n"), std::string_view::npos);
    client.close();
    ::close(listener);
}
