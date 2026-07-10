/// client_demo.cpp
/// Demonstrates the NovaBoot HTTP/3 REST client — both the low-level RestClient
/// API and the declarative RestClientFactory::make<T>() annotation-driven API.
///
/// Run the sample_app server first:
///   ./build/examples/sample_app
///
/// Then in another terminal:
///   ./build/examples/client_demo

#include <iostream>
#include <format>
#include <memory>

#include "novaboot/client/rest_client.h"
#include "novaboot/core/io_uring_event_loop.h"
#include <spdlog/spdlog.h>

int main() {
    spdlog::set_level(spdlog::level::debug);
    std::cout << "=== NovaBoot HTTP/3 REST Client Demo ===\n\n";

    // ─── 1. Create an event loop for the client ────────────────────────────
    // IoUringEventLoop holds large internal recv/send buffers (~10MB) so it
    // MUST be heap-allocated, not placed on the stack.
    auto event_loop = std::make_unique<novaboot::core::IoUringEventLoop>();

    // ─── 2. Low-level RestClient API ──────────────────────────────────────
    std::cout << "--- Low-level RestClient ---\n";

    novaboot::client::RestClient::Config cfg;
    cfg.host       = "localhost";
    cfg.ip         = "127.0.0.1"; // explicit IPv4 — server listens on 0.0.0.0
    cfg.port       = 4433;
    cfg.verify_ssl = false; // self-signed cert in dev

    try {
        auto client = novaboot::client::RestClient::create(cfg, *event_loop);


        // Synchronous GET
        std::cout << "GET /api/users ...\n";
        auto resp = client->get("/api/users");
        std::cout << std::format("  Status: {}\n  Body:   {}\n\n",
                                 resp.status_code, resp.body);

        // Synchronous POST with JSON body
        std::cout << "POST /api/users ...\n";
        const char* new_user = R"({"id":0,"name":"Alice","email":"alice@example.com","age":28})";
        auto post_resp = client->post("/api/users", new_user);
        std::cout << std::format("  Status: {}\n  Body:   {}\n\n",
                                 post_resp.status_code, post_resp.body);

        // Synchronous GET by ID
        std::cout << "GET /api/users/1 ...\n";
        auto user_resp = client->get("/api/users/1");
        std::cout << std::format("  Status: {}\n  Body:   {}\n\n",
                                 user_resp.status_code, user_resp.body);

    } catch (const novaboot::client::ClientError& e) {
        std::cerr << "Client error: " << e.what() << "\n";
        return 1;
    }

    // ─── 3. Coroutine async API demo ──────────────────────────────────────
    std::cout << "--- Async coroutine API ---\n";

    novaboot::client::RestClient::Config async_cfg;
    async_cfg.host       = "localhost";
    async_cfg.ip         = "127.0.0.1";
    async_cfg.port       = 4433;
    async_cfg.verify_ssl = false;

    try {
        auto async_client = novaboot::client::RestClient::create(async_cfg, *event_loop);

        // async_get returns a Task<ClientResponse>. We .await_resume() it here
        // but in a real coroutine you'd `co_await async_client->async_get(...)`.
        auto task = async_client->async_get("/api/users/2");
        // Spin the event loop until the task is done
        while (!task.is_ready()) {
            event_loop->run_once();
        }
        auto result = task.await_resume();
        std::cout << std::format("  Async GET /api/users/2 → status={} body={}\n\n",
                                 result.status_code, result.body);

    } catch (const novaboot::client::ClientError& e) {
        std::cerr << "Async client error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Done!\n";
    return 0;
}
