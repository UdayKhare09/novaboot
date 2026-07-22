#include <gtest/gtest.h>

#include "novaboot/novaboot.h"

namespace stomp = novaboot::messaging::stomp;

struct StompMessageController {
    [[= novaboot::annotations::MessageMapping("/chat.echo") ]]
    [[= novaboot::annotations::SendTo("/topic/chat") ]]
    std::string echo(std::string_view payload) {
        return "echo: " + std::string(payload);
    }
};

namespace {

std::vector<std::uint8_t> masked_text(std::string_view text) {
    constexpr std::array<std::uint8_t, 4> mask{0x11, 0x22, 0x33, 0x44};
    std::vector<std::uint8_t> frame{0x81U, static_cast<std::uint8_t>(0x80U | text.size())};
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t index = 0; index < text.size(); ++index) {
        frame.push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[index]) ^ mask[index % mask.size()]));
    }
    return frame;
}

std::string server_text(const std::vector<std::uint8_t>& frame) {
    EXPECT_EQ(frame[0], 0x81U);
    const auto length = frame[1] & 0x7fU;
    EXPECT_LT(length, 126U);
    return {frame.begin() + 2, frame.end()};
}

std::string stomp_frame(std::string content) {
    content.push_back('\0');
    return content;
}

} // namespace

TEST(StompDecoderTest, ReassemblesFragmentedFramesAndUnescapesHeaders) {
    stomp::Decoder decoder;
    auto first = decoder.feed("SEND\ndestination:/topic/chat\nlabel:hello\\cworld\n\nhel");
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->empty());

    auto second = decoder.feed(std::string_view{"lo\0", 3});
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->size(), 1U);
    EXPECT_EQ((*second)[0].command, "SEND");
    EXPECT_EQ((*second)[0].header("destination"), "/topic/chat");
    EXPECT_EQ((*second)[0].header("label"), "hello:world");
    EXPECT_EQ((*second)[0].body, "hello");
}

TEST(StompDecoderTest, SupportsNulBodiesWhenContentLengthIsPresent) {
    stomp::Frame frame{
        .command = "MESSAGE",
        .headers = {{"destination", "/queue/jobs"}},
        .body = std::string("a\0b", 3),
    };
    stomp::Decoder decoder;
    const auto parsed = decoder.feed(stomp::serialize(frame));

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->size(), 1U);
    EXPECT_EQ((*parsed)[0].body, frame.body);
}

TEST(StompDecoderTest, RejectsInvalidEscapesAndStaysFailed) {
    stomp::Decoder decoder;
    const auto parsed = decoder.feed("SEND\nbad\\x:value\n\n");

    EXPECT_FALSE(parsed.has_value());
    EXPECT_TRUE(decoder.failed());
    EXPECT_FALSE(decoder.feed("\n").has_value());
}

TEST(StompBrokerTest, DeliversMessageFramesToTopicSubscribers) {
    stomp::SimpleBroker broker;
    novaboot::websocket::SessionHandle handle;
    novaboot::websocket::Connection connection({
        .on_open = [&](novaboot::websocket::Session& session) { handle = session.handle(); },
        .on_message = {}, .on_close = {}, .authorize = {},
    });

    ASSERT_TRUE(broker.subscribe("/topic/chat", "chat-1", handle));
    EXPECT_EQ(broker.publish("/topic/chat", "hello", {{"content-type", "text/plain"}}), 1U);
    const auto raw = connection.drain_external_outbound();
    ASSERT_GE(raw.size(), 2U);
    ASSERT_EQ(raw[0], 0x81U);

    const std::string payload(raw.begin() + 2, raw.end());
    stomp::Decoder decoder;
    const auto message = decoder.feed(payload);
    ASSERT_TRUE(message.has_value());
    ASSERT_EQ(message->size(), 1U);
    EXPECT_EQ((*message)[0].command, "MESSAGE");
    EXPECT_EQ((*message)[0].header("destination"), "/topic/chat");
    EXPECT_EQ((*message)[0].header("subscription"), "chat-1");
    EXPECT_EQ((*message)[0].body, "hello");
}

TEST(StompBrokerTest, TracksAndRedeliversClientIndividualAcknowledgements) {
    stomp::SimpleBroker broker;
    novaboot::websocket::SessionHandle handle;
    novaboot::websocket::Connection connection({
        .on_open = [&](novaboot::websocket::Session& session) { handle = session.handle(); },
        .on_message = {}, .on_close = {}, .authorize = {},
    });
    ASSERT_TRUE(broker.subscribe("/queue/jobs", "jobs-1", handle,
                                 stomp::SimpleBroker::AckMode::ClientIndividual));
    ASSERT_EQ(broker.publish("/queue/jobs", "work"), 1U);
    stomp::Decoder decoder;
    auto received = decoder.feed(server_text(connection.drain_external_outbound()));
    ASSERT_TRUE(received.has_value());
    const auto ack_id = (*received)[0].header("ack");
    ASSERT_FALSE(ack_id.empty());
    EXPECT_EQ(broker.pending_ack_count(handle.id()), 1U);

    ASSERT_TRUE(broker.negative_acknowledge(handle.id(), ack_id));
    received = decoder.feed(server_text(connection.drain_external_outbound()));
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ((*received)[0].header("redelivered"), "true");
    EXPECT_TRUE(broker.acknowledge(handle.id(), ack_id));
    EXPECT_EQ(broker.pending_ack_count(handle.id()), 0U);
}

TEST(StompBrokerTest, DistributesQueuesAndRestrictsUserDestinations) {
    stomp::SimpleBroker broker;
    novaboot::websocket::SessionHandle first_handle;
    novaboot::websocket::SessionHandle second_handle;
    novaboot::websocket::Connection first({
        .on_open = [&](novaboot::websocket::Session& session) { first_handle = session.handle(); },
        .on_message = {}, .on_close = {}, .authorize = {},
    });
    novaboot::websocket::Connection second({
        .on_open = [&](novaboot::websocket::Session& session) { second_handle = session.handle(); },
        .on_message = {}, .on_close = {}, .authorize = {},
    });
    ASSERT_TRUE(broker.subscribe("/queue/jobs", "first", first_handle));
    ASSERT_TRUE(broker.subscribe("/queue/jobs", "second", second_handle));
    EXPECT_EQ(broker.publish("/queue/jobs", "one"), 1U);
    EXPECT_EQ(broker.publish("/queue/jobs", "two"), 1U);
    EXPECT_FALSE(first.drain_external_outbound().empty());
    EXPECT_FALSE(second.drain_external_outbound().empty());

    ASSERT_TRUE(broker.subscribe("/user/alice/queue/alerts", "alice", first_handle,
                                 stomp::SimpleBroker::AckMode::Auto, "alice"));
    ASSERT_TRUE(broker.subscribe("/user/alice/queue/alerts", "bob", second_handle,
                                 stomp::SimpleBroker::AckMode::Auto, "bob"));
    EXPECT_EQ(broker.publish("/user/alice/queue/alerts", "private"), 1U);
    EXPECT_FALSE(first.drain_external_outbound().empty());
    EXPECT_TRUE(second.drain_external_outbound().empty());
}

TEST(StompEndpointTest, ConnectsSubscribesAndPublishesThroughTheBroker) {
    stomp::SimpleBroker broker;
    stomp::Endpoint endpoint(broker);
    novaboot::websocket::Connection connection(endpoint.websocket_handler());
    stomp::Decoder decoder;

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame("CONNECT\naccept-version:1.2\n\n"))).has_value());
    auto reply = decoder.feed(server_text(connection.take_outbound()));
    ASSERT_TRUE(reply.has_value());
    ASSERT_EQ(reply->size(), 1U);
    EXPECT_EQ((*reply)[0].command, "CONNECTED");
    EXPECT_EQ((*reply)[0].header("version"), "1.2");

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SUBSCRIBE\nid:chat-1\ndestination:/topic/chat\nack:client-individual\n\n"))).has_value());
    EXPECT_EQ(broker.subscription_count("/topic/chat"), 1U);

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SEND\ndestination:/topic/chat\n\nhello"))).has_value());
    reply = decoder.feed(server_text(connection.drain_external_outbound()));
    ASSERT_TRUE(reply.has_value());
    ASSERT_EQ(reply->size(), 1U);
    EXPECT_EQ((*reply)[0].command, "MESSAGE");
    EXPECT_EQ((*reply)[0].header("subscription"), "chat-1");
    EXPECT_EQ((*reply)[0].body, "hello");
    const auto ack_id = (*reply)[0].header("ack");
    ASSERT_FALSE(ack_id.empty());
    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "ACK\nid:" + std::string(ack_id) + "\n\n"))).has_value());
    EXPECT_TRUE(connection.take_outbound().empty());
}

TEST(StompEndpointTest, NegotiatesAndEmitsHeartbeats) {
    stomp::SimpleBroker broker;
    stomp::Endpoint endpoint(broker, {
        .server_outgoing_heartbeat = std::chrono::milliseconds(50),
        .server_incoming_heartbeat = std::chrono::milliseconds(50),
        .interceptor = {},
    });
    novaboot::websocket::Connection connection(endpoint.websocket_handler());
    stomp::Decoder decoder;

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "CONNECT\naccept-version:1.2\nheart-beat:10,10\n\n"))).has_value());
    auto response = decoder.feed(server_text(connection.take_outbound()));
    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->size(), 1U);
    EXPECT_EQ((*response)[0].header("heart-beat"), "50,50");

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(server_text(connection.drain_external_outbound()), "\n");
}

TEST(StompEndpointTest, DispatchesApplicationDestinations) {
    stomp::SimpleBroker broker;
    stomp::MessageDispatcher dispatcher;
    stomp::MessagingTemplate messaging(broker);
    dispatcher.add_mapping("/chat.send", [&](std::string_view body) {
        return messaging.convert_and_send("/topic/chat", body) == 1U;
    });
    stomp::Endpoint endpoint(broker, {.dispatcher = &dispatcher, .interceptor = {}});
    novaboot::websocket::Connection connection(endpoint.websocket_handler());
    stomp::Decoder decoder;

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame("CONNECT\naccept-version:1.2\n\n"))).has_value());
    (void)connection.take_outbound();
    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SUBSCRIBE\nid:chat\ndestination:/topic/chat\n\n"))).has_value());
    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SEND\ndestination:/app/chat.send\n\nfrom-controller"))).has_value());
    const auto delivered = decoder.feed(server_text(connection.drain_external_outbound()));
    ASSERT_TRUE(delivered.has_value());
    ASSERT_EQ(delivered->size(), 1U);
    EXPECT_EQ((*delivered)[0].body, "from-controller");
}

TEST(StompEndpointTest, UsesConfiguredApplicationDestinationPrefix) {
    stomp::SimpleBroker broker;
    stomp::MessageDispatcher dispatcher;
    std::string received;
    dispatcher.add_mapping("/chat.send", [&](std::string_view body) {
        received = body;
        return true;
    });
    stomp::Endpoint endpoint(broker, {
        .dispatcher = &dispatcher,
        .application_destination_prefix = "/commands",
        .interceptor = {},
    });
    novaboot::websocket::Connection connection(endpoint.websocket_handler());

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame("CONNECT\naccept-version:1.2\n\n"))).has_value());
    (void)connection.take_outbound();
    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SEND\ndestination:/commands/chat.send\n\ncustom-prefix"))).has_value());

    EXPECT_EQ(received, "custom-prefix");
    EXPECT_TRUE(connection.take_outbound().empty());
}

TEST(StompEndpointTest, UsesMessageMappingAndSendToAnnotations) {
    stomp::SimpleBroker broker;
    stomp::MessageDispatcher dispatcher;
    stomp::MessagingTemplate messaging(broker);
    StompMessageController controller;
    novaboot::annotations::register_message_mappings(dispatcher, messaging, controller);
    stomp::Endpoint endpoint(broker, {.dispatcher = &dispatcher, .interceptor = {}});
    novaboot::websocket::Connection connection(endpoint.websocket_handler());
    stomp::Decoder decoder;

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame("CONNECT\naccept-version:1.2\n\n"))).has_value());
    (void)connection.take_outbound();
    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SUBSCRIBE\nid:chat\ndestination:/topic/chat\n\n"))).has_value());
    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SEND\ndestination:/app/chat.echo\n\nhello"))).has_value());
    const auto delivered = decoder.feed(server_text(connection.drain_external_outbound()));
    ASSERT_TRUE(delivered.has_value());
    ASSERT_EQ(delivered->size(), 1U);
    EXPECT_EQ((*delivered)[0].body, "echo: hello");
}

TEST(StompEndpointTest, AppliesFrameInterceptorBeforeBrokerDispatch) {
    stomp::SimpleBroker broker;
    stomp::Endpoint endpoint(broker, {
        .interceptor = [](const novaboot::websocket::Session&, const stomp::Frame& frame) {
            return frame.command != "SEND";
        },
    });
    novaboot::websocket::Connection connection(endpoint.websocket_handler());
    stomp::Decoder decoder;

    ASSERT_TRUE(connection.feed(masked_text(stomp_frame("CONNECT\naccept-version:1.2\n\n"))).has_value());
    (void)connection.take_outbound();
    ASSERT_TRUE(connection.feed(masked_text(stomp_frame(
        "SEND\ndestination:/topic/chat\n\nblocked"))).has_value());
    const auto error = decoder.feed(server_text(connection.take_outbound()));
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->size(), 1U);
    EXPECT_EQ((*error)[0].command, "ERROR");
}

TEST(StompEndpointTest, FrameAuthorizerRestrictsDestinationsAndRequiresPrincipal) {
    const auto authorize = stomp::frame_authorizer({
        .require_authenticated_principal = true,
        .allowed_send_destinations = {"/app"},
        .allowed_subscribe_destinations = {"/topic/public"},
    });
    const auto evaluate = [&](std::string principal, stomp::Frame frame) {
        bool allowed = false;
        novaboot::websocket::Connection connection({
            .on_message = [&](novaboot::websocket::Session& session,
                              const novaboot::websocket::Message&) {
                allowed = authorize(session, frame);
            },
        }, std::move(principal));
        EXPECT_TRUE(connection.feed(masked_text("evaluate")).has_value());
        return allowed;
    };

    EXPECT_FALSE(evaluate({}, {.command = "SEND", .headers = {{"destination", "/app/chat"}}, .body = {}}));
    EXPECT_TRUE(evaluate("alice", {.command = "SEND", .headers = {{"destination", "/app/chat"}}, .body = {}}));
    EXPECT_FALSE(evaluate("alice", {.command = "SUBSCRIBE", .headers = {{"destination", "/topic/private"}}, .body = {}}));
}
