#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/middleware/middleware.h"

#include "repository/user_repository.h"
#include "service/user_service.h"
#include "service/request_logger.h"
#include "controller/user_controller.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <memory>

using namespace novaboot;
using namespace novaboot::di;

// Forward declare the auto-generated CMake registrations function
void novaboot_di_register_all(RootContainer& root);

/// Middleware that creates and binds a request-scoped DI container to each request
class DIMiddleware : public middleware::Middleware {
public:
    explicit DIMiddleware(RootContainer& root) : root_(root) {}

    void handle(http3::Request& req, http3::Response& res,
                context::RequestContext& ctx, Next next) override {
        // Thread-local ShardContainer: one per event loop worker thread (no locks)
        static thread_local std::unique_ptr<ShardContainer> shard_di = nullptr;
        if (!shard_di) {
            shard_di = root_.make_shard_container();
            shard_di->initialize();
        }

        // Create request-scoped container (arena-allocated context)
        auto request_di = shard_di->make_request_container();
        
        // Bind the container to the request context
        ctx.bind_container(*request_di);

        // Run subsequent middleware and route handlers
        next();
    }

private:
    RootContainer& root_;
};

int main() {
    // Set logging level
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting Spring Boot-style C++ DI Sample App");

    // 1. DI Container Setup
    RootContainer di_root;

    // Register all scanned components (UserRepository, UserService, RequestLogger, UserController)
    novaboot_di_register_all(di_root);

    // Build the container: builds dependency graph and instantiates singletons
    di_root.build();

    // 2. Server Setup (configure DIMiddleware for request-scoped DI resolution)
    auto app = Server::create()
        .workers(std::thread::hardware_concurrency())
        .bind("0.0.0.0", 4433)
        .tls("cert.pem", "key.pem")
        .middleware(std::make_shared<DIMiddleware>(di_root))
        .build();

    // 3. Web Routing Mapping to UserController
    // Note: UserController and RequestLogger are resolved automatically from the context!

    // GET /api/users
    app->route("/api/users")
        .get([](http3::Request& req, http3::Response& res, context::RequestContext& ctx) {
            ctx.inject<UserController>().list_users(req, res, ctx);
        });

    // GET /api/users/:id
    app->route("/api/users/:id")
        .get([](http3::Request& req, http3::Response& res, context::RequestContext& ctx) {
            ctx.inject<UserController>().get_user(req, res, ctx);
        });

    // 4. Run the Server
    app->run();

    // Shutdown DI Container to invoke pre-destroy hooks
    di_root.shutdown();
    
    return 0;
}
