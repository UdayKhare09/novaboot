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
#include "config/web_config.h"

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



    // Automatically scan and register repositories, services, controllers, and configurations
    novaboot::annotations::register_beans<
        AppUserRepository, TodoRepository, NoteRepository,
        AuthService, TodoService, NoteService,
        AuthController, TodoController, NoteController,
        GlobalExceptionHandler,
        todo_notes::config::WebConfig
    >(di_root);

    // Build DI Container
    di_root.build();



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
        .build();

    // 7. Run the Server
    app->run();

    // Shutdown DI Container
    di_root.shutdown();
    return 0;
}
