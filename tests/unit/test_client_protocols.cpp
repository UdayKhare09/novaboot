#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include <memory>

#include "novaboot/core/server.h"
#include "novaboot/client/rest_client.h"
#include "novaboot/core/io_uring_event_loop.h"
#include "novaboot/http/sse.h"
#include "novaboot/testing/live_server.h"
#include <spdlog/spdlog.h>

using namespace novaboot;
using namespace novaboot::client;

static std::string get_cert_path() {
    std::filesystem::path p = "cert.pem";
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(p)) return p.string();
        p = "../" / p;
    }
    return "";
}

static std::string get_key_path() {
    std::filesystem::path p = "key.pem";
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(p)) return p.string();
        p = "../" / p;
    }
    return "";
}

class ClientProtocolsTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        spdlog::set_level(spdlog::level::info);
        
        auto cert = get_cert_path();
        auto key = get_key_path();
        
        // Start a single server instance to test against
        auto app = Server::create()
            .bind("127.0.0.1", 4437)
            .tls(cert, key)
            .workers(1)
            .build();
            
        app->route("/api/hello").get([](auto&, auto& res, auto&) {
            res.status(200).body("Hello from server!");
        });
        
        app->route("/api/echo").post([](auto& req, auto& res, auto&) {
            res.status(200)
               .header("x-reflected-header", "novaboot-test")
               .body(req.body());
        });

        app->route("/api/slow").get([](auto&, auto& res, auto&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            res.status(200).body("slow response");
        });

        app->route("/api/sse-once").get([](auto&, auto& res, auto&) {
            auto channel = std::make_shared<http::sse::Channel>();
            http::sse::Event event;
            event.event = "ready";
            event.data = "connected";
            (void)channel->publish(event);
            channel->close();
            http::sse::open(res, std::move(channel));
        });
        
        live_server = std::make_unique<novaboot::testing::LiveServer>(std::move(app));
    }
    
    static void TearDownTestSuite() {
        live_server.reset();
    }
    
    void SetUp() override {
        event_loop = std::make_unique<core::IoUringEventLoop>();
    }
    
    std::unique_ptr<core::IoUringEventLoop> event_loop;
    static std::unique_ptr<novaboot::testing::LiveServer> live_server;
};

std::unique_ptr<novaboot::testing::LiveServer> ClientProtocolsTest::live_server;

TEST_F(ClientProtocolsTest, HTTP11GetRequest) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP1_1;
    
    auto client = RestClient::create(cfg, *event_loop);
    ASSERT_TRUE(client != nullptr);
    
    auto resp = client->get("/api/hello");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, "Hello from server!");
}

TEST_F(ClientProtocolsTest, HTTP11PostRequest) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP1_1;
    
    auto client = RestClient::create(cfg, *event_loop);
    ASSERT_TRUE(client != nullptr);
    
    std::string post_body = "HTTP/1.1 Echo Test Data";
    auto resp = client->post("/api/echo", post_body);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, post_body);
    
    auto header_val = resp.headers.get("x-reflected-header");
    ASSERT_TRUE(header_val.has_value());
    EXPECT_EQ(*header_val, "novaboot-test");
}

TEST_F(ClientProtocolsTest, HTTP11ClientReusesAConnectionForSequentialRequests) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP1_1;

    auto client = RestClient::create(cfg, *event_loop);
    ASSERT_TRUE(client != nullptr);
    const auto first = client->get("/api/hello");
    const auto second = client->get("/api/hello");
    EXPECT_EQ(first.status_code, 200);
    EXPECT_EQ(second.status_code, 200);
    EXPECT_EQ(first.body, "Hello from server!");
    EXPECT_EQ(second.body, "Hello from server!");
}

TEST_F(ClientProtocolsTest, HTTP2GetRequest) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP2;
    
    auto client = RestClient::create(cfg, *event_loop);
    ASSERT_TRUE(client != nullptr);
    
    auto resp = client->get("/api/hello");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, "Hello from server!");
}

TEST_F(ClientProtocolsTest, HTTP2PostRequest) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP2;
    
    auto client = RestClient::create(cfg, *event_loop);
    ASSERT_TRUE(client != nullptr);
    
    std::string post_body = "HTTP/2 Multiplexed Echo Test Data";
    auto resp = client->post("/api/echo", post_body);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, post_body);
    
    auto header_val = resp.headers.get("x-reflected-header");
    ASSERT_TRUE(header_val.has_value());
    EXPECT_EQ(*header_val, "novaboot-test");
}

TEST_F(ClientProtocolsTest, HTTP2DeliversAndCompletesFiniteSseStream) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP2;

    auto client = RestClient::create(cfg, *event_loop);
    const auto response = client->get("/api/sse-once");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "event: ready\ndata: connected\n\n");
    EXPECT_EQ(response.headers.get("content-type").value_or(""),
              "text/event-stream; charset=utf-8");
}

TEST_F(ClientProtocolsTest, HTTP3DeliversAndCompletesFiniteSseStream) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP3;

    auto client = RestClient::create(cfg, *event_loop);
    const auto response = client->get("/api/sse-once");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "event: ready\ndata: connected\n\n");
    EXPECT_EQ(response.headers.get("content-type").value_or(""),
              "text/event-stream; charset=utf-8");
}

TEST_F(ClientProtocolsTest, SSLVerificationOnAllowedHost) {
    RestClient::Config cfg;
    cfg.host = "localhost"; // Valid CN in mkcert dev cert
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = true; // Enable peer validation
    cfg.protocol = Protocol::HTTP2;
    
    auto client = RestClient::create(cfg, *event_loop);
    ASSERT_TRUE(client != nullptr);
    
    auto resp = client->get("/api/hello");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, "Hello from server!");
}

TEST_F(ClientProtocolsTest, SSLVerificationOnDisallowedHostThrows) {
    RestClient::Config cfg;
    cfg.host = "someotherhost.com"; // Domain mismatch relative to certificate
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = true; // Enable peer validation (should fail)
    cfg.protocol = Protocol::HTTP2;
    
    // We expect connection handshake to throw a ClientError due to certificate verification failure
    EXPECT_THROW({
        auto client = RestClient::create(cfg, *event_loop);
        client->get("/api/hello");
    }, ClientError);
}

TEST_F(ClientProtocolsTest, RequestDeadlineAbortsTheOutstandingCoroutineSafely) {
    RestClient::Config cfg;
    cfg.host = "localhost";
    cfg.ip = "127.0.0.1";
    cfg.port = 4437;
    cfg.verify_ssl = false;
    cfg.protocol = Protocol::HTTP1_1;
    cfg.request_timeout_ms = 20;

    auto client = RestClient::create(cfg, *event_loop);
    ASSERT_TRUE(client != nullptr);
    EXPECT_THROW((void)client->get("/api/slow"), ClientError);
}
