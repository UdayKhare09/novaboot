#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/middleware/middleware.h"
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
#include "service/auth_service.h"
#include "service/todo_service.h"
#include "service/note_service.h"
#include "controller/auth_controller.h"
#include "controller/todo_controller.h"
#include "controller/note_controller.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <memory>

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::data;
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

    // Register data sources
    di_root.singleton<PgsqlDataSource>([](ContainerBase& c) {
        return new PgsqlDataSource(c.resolve<AppConfig>().postgres());
    }).depends_on<AppConfig>();

    // Repositories
    di_root.autowire<AppUserRepository>();
    di_root.autowire<TodoRepository>();
    di_root.autowire<NoteRepository>();

    // Services
    di_root.autowire<AuthService>();
    di_root.autowire<TodoService>();
    di_root.autowire<NoteService>();

    // Controllers
    di_root.autowire<AuthController>();
    di_root.autowire<TodoController>();
    di_root.autowire<NoteController>();

    // Build DI Container
    di_root.build();

    // 2. Programmatically initialize tables in PostgreSQL
    {
        auto& ds = di_root.resolve<PgsqlDataSource>();
        try {
            ds.transact([](auto& db) {
                // Clean up any legacy integer-based schemas so they are recreated as UUID strings
                db.execute("DROP TABLE IF EXISTS todos;");
                db.execute("DROP TABLE IF EXISTS notes;");
                db.execute("DROP TABLE IF EXISTS app_users;");

                db.execute("CREATE TABLE IF NOT EXISTS app_users ("
                           "id TEXT PRIMARY KEY, "
                           "username TEXT NOT NULL UNIQUE, "
                           "password_hash TEXT NOT NULL, "
                           "email TEXT NOT NULL"
                           ");");

                db.execute("CREATE TABLE IF NOT EXISTS todos ("
                           "id SERIAL PRIMARY KEY, "
                           "user_id TEXT NOT NULL, "
                           "title TEXT NOT NULL, "
                           "description TEXT, "
                           "completed BOOLEAN NOT NULL DEFAULT FALSE"
                           ");");

                db.execute("CREATE TABLE IF NOT EXISTS notes ("
                           "id SERIAL PRIMARY KEY, "
                           "user_id TEXT NOT NULL, "
                           "title TEXT NOT NULL, "
                           "content TEXT"
                           ");");
            });
            spdlog::info("Database schema checks completed (tables initialized if not exist).");
        } catch (const std::exception& e) {
            spdlog::error("Failed to check/initialize database schema: {}", e.what());
            return 1;
        }
    }

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
    app->router().group("/public")
        .post("/register", di::handler<&todo_notes::controller::AuthController::register_user>())
        .post("/login", di::handler<&todo_notes::controller::AuthController::login_user>());

    app->router().group("/api/todos")
        .get("", di::handler<&todo_notes::controller::TodoController::list_todos>())
        .get("/:id", di::handler<&todo_notes::controller::TodoController::get_todo>())
        .post("", di::handler<&todo_notes::controller::TodoController::create_todo>())
        .put("/:id", di::handler<&todo_notes::controller::TodoController::update_todo>())
        .del("/:id", di::handler<&todo_notes::controller::TodoController::delete_todo>());

    app->router().group("/api/notes")
        .get("", di::handler<&todo_notes::controller::NoteController::list_notes>())
        .get("/:id", di::handler<&todo_notes::controller::NoteController::get_note>())
        .post("", di::handler<&todo_notes::controller::NoteController::create_note>())
        .put("/:id", di::handler<&todo_notes::controller::NoteController::update_note>())
        .del("/:id", di::handler<&todo_notes::controller::NoteController::delete_note>());

    // 6. Global Exception Handlers mapping to JSON responses
    app->on_exception<std::runtime_error>(
        [](const std::runtime_error& ex, context::RequestContext&) {
            ErrorResponse err{"Bad Request", ex.what()};
            return ResponseEntity<ErrorResponse>::status(400, err);
        }
    );

    app->on_exception<novaboot::validation::ValidationException>(
        [](const novaboot::validation::ValidationException& ex, context::RequestContext&) {
            std::string msg;
            for (const auto& err : ex.errors()) {
                if (!msg.empty()) msg += ", ";
                msg += err;
            }
            ErrorResponse err{"Validation Failed", msg};
            return ResponseEntity<ErrorResponse>::status(400, err);
        }
    );

    // 7. Run the Server
    app->run();

    // Shutdown DI Container
    di_root.shutdown();
    return 0;
}
