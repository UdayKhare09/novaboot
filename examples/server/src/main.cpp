#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/config/app_config.h"

// ── Built-in library middleware ──────────────────────────────────────────────
#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/authorization_middleware.h"
#include "novaboot/middleware/body_size_limit_middleware.h"
#include "novaboot/middleware/compression_middleware.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"
#include "novaboot/middleware/security_headers_middleware.h"

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
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("Starting Spring Boot-style C++ DI Sample App with TOML Config");

    // 0. Load TOML configuration
    auto cfg = novaboot::config::AppConfig::load("examples/server/src/resources/config.toml");

    // 1. DI Container Setup
    RootContainer di_root;

    // Register pre-loaded AppConfig in the container so modules can resolve it
    di_root.register_bean<novaboot::config::AppConfig>([cfg](ContainerBase&) {
        return new novaboot::config::AppConfig(cfg);
    });

    // Explicitly register all components and database sources in the container
    di_root.singleton<novaboot::data::PgsqlDataSource>([](auto& c) {
        return new novaboot::data::PgsqlDataSource(c.template resolve<novaboot::config::AppConfig>().postgres());
    });
    di_root.singleton<novaboot::data::RedisDataSource>([](auto& c) {
        return new novaboot::data::RedisDataSource(c.template resolve<novaboot::config::AppConfig>().redis());
    });

    di_root.request<RequestLogger>([](auto&) {
        return new RequestLogger();
    });

    di_root.singleton<UserSqlRepository>([](auto& c) {
        return new UserSqlRepository(c.template resolve<novaboot::data::PgsqlDataSource>());
    });
    di_root.bind<novaboot::data::CrudRepository<examples::model::User, int>>().to<UserSqlRepository>();

    di_root.singleton<UserCacheRepository>([](auto& c) {
        return new UserCacheRepository(c.template resolve<novaboot::data::RedisDataSource>());
    });
    di_root.bind<novaboot::data::CacheRepository<examples::model::User, int>>().to<UserCacheRepository>();

    di_root.singleton<UserRepository>([](auto& c) {
        return new UserRepository(
            c.template resolve<novaboot::data::CrudRepository<examples::model::User, int>>(),
            c.template resolve<novaboot::data::CacheRepository<examples::model::User, int>>()
        );
    });

    di_root.singleton<UserService>([](auto& c) {
        return new UserService(c.template resolve<UserRepository>());
    })
    .on_start(&UserService::init)
    .on_stop(&UserService::cleanup);

    di_root.singleton<UserController>([](auto& c) {
        return new UserController(c.template resolve<UserService>());
    });

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

    // (c) Security headers — added even when downstream middleware short-circuits.
    auto security_headers =
        std::make_shared<novaboot::middleware::SecurityHeadersMiddleware>();

    // (d) Body-size limit — rejects oversized request bodies before auth/handlers.
    auto body_limit = std::make_shared<novaboot::middleware::BodySizeLimitMiddleware>(
        novaboot::middleware::BodySizeLimitMiddleware::Config{
            .max_body_bytes = 64 * 1024,
            .allowlist_paths = {},
        });

    // (e) JWT auth — protects everything except explicitly public routes.
    auto jwt = std::make_shared<novaboot::middleware::JwtMiddleware>(
        novaboot::middleware::JwtMiddleware::Config{
            .allowed_algorithms = {
                novaboot::middleware::JwtMiddleware::Algorithm::HS256,
            },
            .hmac_secret = "sample-secret",
            .rsa_public_key_pem = "",
            .allowlist_paths = {"/", "/public/*"},
            .required_issuer = "novaboot-sample",
            .required_audiences = {"sample-api"},
            .required_scopes = {"read"},
        });

    // (f) Authorization — applies route-level permissions after JWT auth.
    auto authorization =
        std::make_shared<novaboot::middleware::AuthorizationMiddleware>(
            novaboot::middleware::AuthorizationMiddleware::Config{
                .policies = {
                    {
                        .path = "/api/users*",
                        .method = {"POST"},
                        .required_scopes = {"write"},
                    },
                    {
                        .path = "/api/*",
                        .method = {"GET", "POST", "PUT", "DELETE"},
                        .required_scopes = {"read"},
                    },
                },
            });

    // (g) Compression — gzips eligible responses when the client asks for it.
    auto compression =
        std::make_shared<novaboot::middleware::CompressionMiddleware>();

    // (h) Request logging — logs after the handler so the status code is known.
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
        // ── middleware order:
        //    CORS → RequestId → SecurityHeaders → BodyLimit → JWT
        //    → Authorization → Compression → Logging → handler
        .middleware(cors)
        .middleware(request_id)
        .middleware(security_headers)
        .middleware(body_limit)
        .middleware(jwt)
        .middleware(authorization)
        .middleware(compression)
        .middleware(logger)
        .build();

    // 4. Web Routing Mapping
    app->router().group("/api/users")
        .get("", di::handler<&UserController::list_users>())
        .get("/:id", di::handler<&UserController::get_user>())
        .post("", di::handler<&UserController::create_user>())
        .put("/:id", di::handler<&UserController::update_user>())
        .del("/:id", di::handler<&UserController::delete_user>())
        .patch("/:id", di::handler<&UserController::patch_user>());

    app->on_exception<examples::exception::UserNotFoundException>(
        [](const examples::exception::UserNotFoundException& ex, novaboot::context::RequestContext&) {
            ErrorResponse err{"User Not Found", ex.what()};
            return novaboot::ResponseEntity<ErrorResponse>::status(404, err);
        }
    );

    // 5. Run the Server
    app->run();

    // Shutdown DI Container to invoke pre-destroy hooks
    di_root.shutdown();
    
    return 0;
}
