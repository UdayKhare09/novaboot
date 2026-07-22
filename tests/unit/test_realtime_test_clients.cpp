#include <gtest/gtest.h>

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
