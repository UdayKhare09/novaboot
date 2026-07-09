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

// ─── Domain types ─────────────────────────────────────────────────────────────

/// Simple in-memory user store (repository stereotype)
struct [[=novaboot::di::repository{}]] UserRepository {
    std::string find(int id) {
        return std::format(R"({{"id":{},"name":"User {}","email":"user{}@example.com"}})",
                           id, id, id);
    }
};

/// Business logic service (service stereotype)
struct [[=novaboot::di::service{}]] UserService {
    UserRepository& repo_;

    /// Constructor injection: DI container detects the single constructor
    explicit UserService(UserRepository& repo) : repo_(repo) {}

    std::string get_user(int id) { return repo_.find(id); }
    std::string list_users()     { return R"([{"id":1},{"id":2},{"id":3}])"; }

    [[=novaboot::di::post_construct{}]]
    void on_start() { spdlog::info("UserService ready"); }

    [[=novaboot::di::pre_destroy{}]]
    void on_stop()  { spdlog::info("UserService shutting down"); }
};

/// Request-scoped logger (one per HTTP request)
struct [[=novaboot::di::component{}]]
       [[=novaboot::di::scoped{novaboot::di::Scope::Request}]]
RequestLogger {
    std::vector<std::string> events;
    void log(std::string_view msg) { events.emplace_back(msg); }
};

// ─── Compile-time DI verification ─────────────────────────────────────────────

static_assert(novaboot::di::detail::is_managed_component(^^UserRepository));
static_assert(novaboot::di::detail::is_managed_component(^^UserService));
static_assert(novaboot::di::detail::get_scope(^^RequestLogger) == Scope::Request);

// Verify UserService depends on UserRepository
consteval bool user_service_dep_check() {
    auto ctor  = novaboot::di::detail::find_inject_ctor(^^UserService);
    auto deps  = novaboot::di::detail::collect_dep_types(ctor);
    return deps.size() == 1 && deps[0] == ^^UserRepository;
}
static_assert(user_service_dep_check(), "UserService must depend on UserRepository");

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("NovaBoot Phase 2 — DI/IoC Demo");

    // ── DI Container setup ────────────────────────────────────────────────────
    RootContainer di_root;

    // Register Singleton beans
    di_root.register_bean<UserRepository>(
        [](ContainerBase&) { return new UserRepository{}; }
    );
    di_root.register_bean<UserService>(
        [](ContainerBase& c) { return new UserService{c.resolve<UserRepository>()}; }
    );
    di_root.with_post_construct<UserService>([](UserService& s) { s.on_start(); });
    di_root.with_pre_destroy<UserService>([](UserService& s)   { s.on_stop();  });

    // Build validates the dependency graph and instantiates all singletons
    di_root.build();

    // ── Server setup ──────────────────────────────────────────────────────────
    auto app = Server::create()
        .workers(std::thread::hardware_concurrency())
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
