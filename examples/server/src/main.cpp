#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/config/app_config.h"

// ── Built-in library middleware ──────────────────────────────────────────────
#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"

// ── Custom in-app middleware ─────────────────────────────────────────────────
#include "middleware/request_id_middleware.h"

#include "repository/user_repository.h"
#include "service/user_service.h"
#include "service/request_logger.h"
#include "controller/user_controller.h"
#include "controller/global_exception_handler.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <memory>

using namespace novaboot;
using namespace novaboot::di;

int main() {
    // Set logging level
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting Spring Boot-style C++ DI Sample App with TOML Config");

    // 0. Load TOML configuration
    auto cfg = novaboot::config::AppConfig::load("examples/server/src/resources/config.toml");

    // 1. DI Container Setup
    RootContainer di_root;

    // Register pre-loaded AppConfig in the container so modules can resolve it
    di_root.register_bean<novaboot::config::AppConfig>([cfg](ContainerBase&) {
        return new novaboot::config::AppConfig(cfg);
    });

    // Register all scanned components (UserRepository, UserService, RequestLogger, UserController, GlobalExceptionHandler)
    novaboot_di_register_all(di_root);

    // Build the container: builds dependency graph and instantiates singletons
    di_root.build();

    // 2. Middleware setup ─────────────────────────────────────────────────────

    // (a) CORS — must be first so preflight OPTIONS requests are handled before
    //     anything else touches the response.
    auto cors = std::make_shared<novaboot::middleware::CorsMiddleware>(
        novaboot::middleware::CorsMiddleware::Config{
            .allowed_origins   = {"*"},
            .allowed_methods   = {"GET","POST","PUT","DELETE","PATCH","OPTIONS"},
            .allowed_headers   = {"Content-Type","Authorization","X-Request-Id"},
            .allow_credentials = false,
            .max_age_seconds   = 86400,
        });

    // (b) Request-ID — stamps every request with a trace ID before the handler.
    auto request_id = std::make_shared<RequestIdMiddleware>();

    // (c) Request logging — logs after the handler so the status code is known.
    auto logger = std::make_shared<novaboot::middleware::RequestLoggingMiddleware>();

    // 3. Server Setup (pass di_root for automatic request-scoped DI resolution)
    int worker_count = static_cast<int>(cfg.server().workers);
    if (worker_count == 0) {
        worker_count = static_cast<int>(std::thread::hardware_concurrency());
    }

    auto app = Server::create()
        .workers(worker_count)
        .bind(cfg.server().host, cfg.server().port)
        .tls(cfg.server().tls_cert, cfg.server().tls_key)
        .di_container(di_root)
        .static_resources(cfg.server().static_resources)
        // ── middleware order: CORS → RequestId → Logging → handler ──
        .middleware(cors)
        .middleware(request_id)
        .middleware(logger)
        .build();

    // 4. Web Routing Mapping (Fully automatic Component Scan routing mapping)
    novaboot_web_register_all(*app);

    // 5. Run the Server
    app->run();

    // Shutdown DI Container to invoke pre-destroy hooks
    di_root.shutdown();
    
    return 0;
}
