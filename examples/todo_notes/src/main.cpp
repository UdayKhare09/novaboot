#include "novaboot/config/app_config.h"

// Middleware
#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/security_headers_middleware.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"

// Repositories, Services, Controllers
#include "repository/app_user_repository.h"
#include "repository/todo_repository.h"
#include "repository/note_repository.h"
#include "controller/auth_controller.h"
#include "controller/todo_controller.h"
#include "controller/note_controller.h"
#include "controller/global_exception_handler.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <memory>

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::config;
using namespace novaboot::middleware;

using namespace todo_notes::service;
using namespace todo_notes::controller;
using todo_notes::model::ErrorResponse;

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting Todo & Notes Application with JWT Auth...");

    // 0. Load TOML configuration
    auto cfg = AppConfig::load("examples/todo_notes/src/resources/config.toml");

    // 1. DI Container Setup
    RootContainer di_root;

    // Register config
    di_root.register_bean<AppConfig>([cfg](ContainerBase&) {
        return new AppConfig(cfg);
    });



    // Automatically scan and register repositories, services, and controllers
    novaboot::annotations::register_beans<
        AppUserRepository, TodoRepository, NoteRepository,
        AuthService, TodoService, NoteService,
        AuthController, TodoController, NoteController,
        GlobalExceptionHandler
    >(di_root);

    // Build DI Container
    di_root.build();



    // 3. Middleware setup
    auto cors = std::make_shared<CorsMiddleware>(
        CorsMiddleware::Config{
            .allowed_origins   = {"*"},
            .allowed_methods   = {"GET","POST","PUT","DELETE","PATCH","OPTIONS"},
            .allowed_headers   = {"Content-Type","Authorization"},
            .allow_credentials = false,
            .max_age_seconds   = 86400,
        });

    auto security_headers = std::make_shared<SecurityHeadersMiddleware>();

    auto jwt = std::make_shared<JwtMiddleware>(
        JwtMiddleware::Config{
            .allowed_algorithms = { JwtMiddleware::Algorithm::HS256 },
            .hmac_secret = "sample-secret",
            .allowlist_paths = {"/", "/index.html", "/public/*"},
            .required_issuer = "novaboot-sample",
            .required_audiences = {"sample-api"},
            .required_scopes = {"read"},
        });

    auto logger = std::make_shared<RequestLoggingMiddleware>();

    // 4. Server setup
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
        .middleware(cors)
        .middleware(security_headers)
        .middleware(jwt)
        .middleware(logger)
        .build();

    // 5. Routing Mapping
    novaboot::annotations::register_routes<AuthController, TodoController, NoteController>(app->router());
    novaboot::annotations::register_advice<GlobalExceptionHandler>(app->router());

    // 7. Run the Server
    app->run();

    // Shutdown DI Container
    di_root.shutdown();
    return 0;
}
