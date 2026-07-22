#include <gtest/gtest.h>

#include "novaboot/http/sse.h"
#include "novaboot/http1/http1_session.h"
#include "novaboot/http2/http2_session.h"

#include <nghttp2/nghttp2.h>

#include <stdexcept>

namespace {

using novaboot::http::Response;
using novaboot::http::sse::Channel;
using novaboot::http::sse::Event;

struct H2SsePeer {
    nghttp2_session* session = nullptr;
    std::string status;
    std::string data;

    H2SsePeer() {
        nghttp2_session_callbacks* callbacks = nullptr;
        if (nghttp2_session_callbacks_new(&callbacks) != 0) {
            throw std::runtime_error("could not allocate HTTP/2 callbacks");
        }
        nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
        if (nghttp2_session_client_new2(&session, callbacks, this, nullptr) != 0) {
            nghttp2_session_callbacks_del(callbacks);
            throw std::runtime_error("could not create HTTP/2 client session");
        }
        nghttp2_session_callbacks_del(callbacks);
    }
    ~H2SsePeer() { if (session) nghttp2_session_del(session); }
    H2SsePeer(const H2SsePeer&) = delete;

    std::vector<std::uint8_t> take_outbound() {
        std::vector<std::uint8_t> output;
        const std::uint8_t* bytes = nullptr;
        while (const auto count = nghttp2_session_mem_send2(session, &bytes)) {
            if (count < 0) return {};
            output.insert(output.end(), bytes, bytes + count);
        }
        return output;
    }
    void receive(const std::vector<std::uint8_t>& bytes) {
        ASSERT_GE(nghttp2_session_mem_recv2(session, bytes.data(), bytes.size()), 0);
    }
    void submit_get() {
        const auto nv = [](std::string_view name, std::string_view value) {
            return nghttp2_nv{
                .name = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(name.data())),
                .value = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(value.data())),
                .namelen = name.size(), .valuelen = value.size(), .flags = NGHTTP2_NV_FLAG_NONE};
        };
        const std::array headers{nv(":method", "GET"), nv(":scheme", "https"),
            nv(":authority", "example.test"), nv(":path", "/events")};
        ASSERT_EQ(nghttp2_submit_headers(session, NGHTTP2_FLAG_END_STREAM, -1, nullptr,
                                         headers.data(), headers.size(), nullptr), 1);
    }
    void submit_settings() {
        ASSERT_EQ(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0), 0);
    }
    static int on_header(nghttp2_session*, const nghttp2_frame* frame,
                         const std::uint8_t* name, std::size_t name_len,
                         const std::uint8_t* value, std::size_t value_len,
                         std::uint8_t, void* user_data) {
        auto* peer = static_cast<H2SsePeer*>(user_data);
        if (frame->hd.stream_id == 1 &&
            std::string_view(reinterpret_cast<const char*>(name), name_len) == ":status") {
            peer->status.assign(reinterpret_cast<const char*>(value), value_len);
        }
        return 0;
    }
    static int on_data(nghttp2_session*, std::uint8_t, int32_t,
                       const std::uint8_t* bytes, std::size_t length, void* user_data) {
        static_cast<H2SsePeer*>(user_data)->data.append(
            reinterpret_cast<const char*>(bytes), length);
        return 0;
    }
};

TEST(Sse, EncodesFieldsAndMultilineData) {
    Event event{
        .data = "first\nsecond",
        .event = "notice",
        .id = "42",
        .retry_milliseconds = 1500,
        .comment = "keepalive",
    };
    const auto encoded = novaboot::http::sse::encode(event);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(*encoded,
              ": keepalive\n"
              "event: notice\n"
              "id: 42\n"
              "retry: 1500\n"
              "data: first\n"
              "data: second\n\n");
}

TEST(Sse, RejectsUnsafeIdAndEventName) {
    Event invalid_id;
    invalid_id.data = "x";
    invalid_id.id = "bad\nvalue";
    EXPECT_FALSE(novaboot::http::sse::encode(invalid_id).has_value());

    Event invalid_name;
    invalid_name.data = "x";
    invalid_name.event = "bad\rvalue";
    EXPECT_FALSE(novaboot::http::sse::encode(invalid_name).has_value());
}

TEST(Sse, ChannelBoundsAndDrainsInOrder) {
    Channel channel({.max_pending_events = 2, .max_pending_bytes = 64});
    Event one;
    one.data = "one";
    Event two;
    two.data = "two";
    Event three;
    three.data = "three";
    EXPECT_EQ(channel.publish(one), Channel::PublishResult::Accepted);
    EXPECT_EQ(channel.publish(two), Channel::PublishResult::Accepted);
    EXPECT_EQ(channel.publish(three), Channel::PublishResult::Backpressured);
    EXPECT_EQ(channel.take_next(), std::optional<std::string>{"data: one\n\n"});
    EXPECT_EQ(channel.take_next(), std::optional<std::string>{"data: two\n\n"});
    EXPECT_FALSE(channel.take_next().has_value());
}

TEST(Sse, ClosePreventsNewEventsAndWakesTransport) {
    Channel channel;
    int wakeups = 0;
    channel.set_wakeup([&wakeups] { ++wakeups; });
    Event ready;
    ready.data = "ready";
    EXPECT_EQ(channel.publish(ready), Channel::PublishResult::Accepted);
    channel.close();
    Event late;
    late.data = "late";
    EXPECT_EQ(channel.publish(late), Channel::PublishResult::Closed);
    EXPECT_EQ(wakeups, 2);
}

TEST(Sse, ConfiguresResponseWithoutContentLength) {
    Response response;
    novaboot::http::sse::configure_response(response);
    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(response.headers().get("content-type"),
              std::optional<std::string_view>{"text/event-stream; charset=utf-8"});
    EXPECT_EQ(response.headers().get("cache-control"),
              std::optional<std::string_view>{"no-cache"});
    EXPECT_FALSE(response.headers().has("content-length"));
}

TEST(Sse, Http11StreamsQueuedEventsAndTerminatesAfterClose) {
    auto channel = std::make_shared<Channel>();
    Event ready;
    ready.data = "ready";
    ASSERT_EQ(channel->publish(ready), Channel::PublishResult::Accepted);

    novaboot::http1::Http1Session session(
        [channel](novaboot::http3::Request&, Response& response) {
            novaboot::http::sse::open(response, channel);
        });
    const auto passthrough = [](const std::vector<std::uint8_t>& bytes) {
        return bytes;
    };
    const std::string request = "GET /events HTTP/1.1\r\nHost: example.test\r\n\r\n";
    const auto initial = session.feed_data(
        {request.begin(), request.end()}, passthrough);
    ASSERT_TRUE(initial.has_value());
    const std::string initial_text(initial->begin(), initial->end());
    EXPECT_NE(initial_text.find("content-type: text/event-stream; charset=utf-8\r\n"),
              std::string::npos);
    EXPECT_NE(initial_text.find("Transfer-Encoding: chunked\r\n"), std::string::npos);
    EXPECT_NE(initial_text.find("d\r\ndata: ready\n\n\r\n"), std::string::npos);

    Event update;
    update.data = "update";
    ASSERT_EQ(channel->publish(update), Channel::PublishResult::Accepted);
    const auto next = session.drain_sse_outbound(passthrough);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(std::string(next->begin(), next->end()), "e\r\ndata: update\n\n\r\n");

    channel->close();
    const auto terminal = session.drain_sse_outbound(passthrough);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(std::string(terminal->begin(), terminal->end()), "0\r\n\r\n");
    EXPECT_FALSE(session.keep_alive());
}

TEST(Sse, Http11DisconnectClosesPublisherChannel) {
    auto channel = std::make_shared<Channel>();
    {
        novaboot::http1::Http1Session session(
            [channel](novaboot::http3::Request&, Response& response) {
                novaboot::http::sse::open(response, channel);
            });
        const auto passthrough = [](const std::vector<std::uint8_t>& bytes) {
            return bytes;
        };
        const std::string request = "GET /events HTTP/1.1\r\nHost: example.test\r\n\r\n";
        ASSERT_TRUE(session.feed_data({request.begin(), request.end()}, passthrough).has_value());
    }

    Event late;
    late.data = "late";
    EXPECT_TRUE(channel->closed());
    EXPECT_EQ(channel->publish(late), Channel::PublishResult::Closed);
}

TEST(Sse, Http2DefersAndResumesEventData) {
    auto channel = std::make_shared<Channel>();
    int wakeups = 0;
    novaboot::http2::Http2Session server(
        [channel](novaboot::http3::Request&, Response& response) {
            novaboot::http::sse::open(response, channel);
        }, {}, [&wakeups] { ++wakeups; });
    const auto passthrough = [](const std::vector<std::uint8_t>& bytes) { return bytes; };
    H2SsePeer peer;
    peer.submit_settings();
    auto settings_reply = server.feed_data(peer.take_outbound(), passthrough);
    ASSERT_TRUE(settings_reply.has_value());
    peer.receive(*settings_reply);
    ASSERT_TRUE(server.feed_data(peer.take_outbound(), passthrough).has_value());
    peer.submit_get();
    auto reply = server.feed_data(peer.take_outbound(), passthrough);
    ASSERT_TRUE(reply.has_value());
    peer.receive(*reply);
    EXPECT_EQ(peer.status, "200");
    EXPECT_TRUE(peer.data.empty());

    Event event;
    event.data = "h2-ready";
    ASSERT_EQ(channel->publish(event), Channel::PublishResult::Accepted);
    EXPECT_EQ(wakeups, 1);
    peer.receive(server.drain_outbound(passthrough));
    EXPECT_EQ(peer.data, "data: h2-ready\n\n");

    channel->close();
    peer.receive(server.drain_outbound(passthrough));
}

} // namespace
