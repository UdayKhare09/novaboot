#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include "novaboot/core/server.h"

using namespace novaboot;

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

TEST(ServerIntegrationTest, BuildAndLifecycle) {
    auto cert = get_cert_path();
    auto key = get_key_path();
    ASSERT_FALSE(cert.empty()) << "cert.pem not found!";
    ASSERT_FALSE(key.empty()) << "key.pem not found!";

    auto app = Server::create()
        .bind("127.0.0.1", 4434)
        .tls(cert, key)
        .workers(1)
        .backend(core::EventLoopBackend::IoUring)
        .build();

    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->worker_count(), 1);

    // Register a simple route
    app->route("/api/test").get([](auto&, auto&, auto&) {});

    // Run the server in a separate thread
    std::thread server_thread([&app]() {
        app->run();
    });

    // Wait a brief moment for startup
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Request shutdown
    app->stop();

    if (server_thread.joinable()) {
        server_thread.join();
    }
}


TEST(ServerIntegrationTest, StaticResourcesConfiguration) {
    auto cert = get_cert_path();
    auto key = get_key_path();
    ASSERT_FALSE(cert.empty()) << "cert.pem not found!";
    ASSERT_FALSE(key.empty()) << "key.pem not found!";

    auto app = Server::create()
        .bind("127.0.0.1", 4436)
        .tls(cert, key)
        .workers(1)
        .static_resources("examples/server/src/resources/static")
        .build();

    ASSERT_NE(app, nullptr);
}
