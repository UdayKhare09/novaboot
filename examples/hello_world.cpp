/// @file hello_world.cpp
/// NovaBoot Phase 2 — DI/IoC demo
///
/// Demonstrates Spring Boot-style dependency injection:
///   - Annotated components (service, repository)
///   - Constructor injection (auto-deduced from constructor signature)
///   - Request-scoped beans
///   - Lifecycle callbacks (post_construct / pre_destroy)
///   - ctx.inject<T>() in route handlers
///
/// Build:
///   cmake -B build -DCMAKE_BUILD_TYPE=Debug -DNOVABOOT_BUILD_EXAMPLES=ON
///   cmake --build build
///
/// Run (requires cert.pem and key.pem):
///   ./build/examples/hello_world

#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"

#include <spdlog/spdlog.h>
#include <string>
#include <format>
#include <thread>

using namespace novaboot;
using namespace novaboot::di;

#include "hello_world_components.h"

// Forward declare the auto-generated CMake registrations function
void novaboot_di_register_all(novaboot::di::RootContainer& root);

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("NovaBoot Phase 2 — DI/IoC Demo");

    // ── DI Container setup ────────────────────────────────────────────────────
    RootContainer di_root;

    // Register beans via CMake component scanner (fully automatic)
    novaboot_di_register_all(di_root);

    // Build validates the dependency graph and instantiates all singletons
    di_root.build();

    // ── Server setup ──────────────────────────────────────────────────────────
    auto app = Server::create()
        .workers((int)std::thread::hardware_concurrency())
        .bind("0.0.0.0", 4433)
        .tls("cert.pem", "key.pem")
        .build();

    // Create a shard-level DI container
    auto shard_di = di_root.make_shard_container();

    // ── Routes ────────────────────────────────────────────────────────────────

    // GET /api/users  — lists all users
    app->route("/api/users")
        .get([&di_root](http3::Request&, http3::Response& res,
                         context::RequestContext& ctx) {
            auto& svc = di_root.resolve<UserService>();
            res.status(200)
               .header("Content-Type", "application/json")
               .body(svc.list_users());
        });

    // GET /api/users/:id  — get a single user by ID
    app->route("/api/users/:id")
        .get([&di_root](http3::Request& req, http3::Response& res,
                         context::RequestContext& ctx) {
            auto id  = req.path_params().get_as<int>("id").value_or(0);
            auto& svc = di_root.resolve<UserService>();
            res.status(200)
               .header("Content-Type", "application/json")
               .body(svc.get_user(id));
        });

    // GET /api/health
    app->route("/api/health")
        .get([](http3::Request&, http3::Response& res, context::RequestContext&) {
            res.status(200)
               .header("Content-Type", "application/json")
               .body(R"({"status":"ok","version":"2.0.0-di"})");
        });

    spdlog::info("NovaBoot listening on https://localhost:4433");
    spdlog::info("Routes: GET /api/users, GET /api/users/:id, GET /api/health");
    spdlog::info("Hit Ctrl+C to stop");

    app->run();

    di_root.shutdown();
    return 0;
}
