#include <gtest/gtest.h>

#include <chrono>
#include <thread>

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
