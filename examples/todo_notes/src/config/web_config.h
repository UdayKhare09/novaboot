#pragma once
#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/config/app_config.h"
#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/security_headers_middleware.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"
#include <memory>

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;
using namespace novaboot::middleware;

namespace todo_notes::config {

struct [[= Configuration() ]] WebConfig {
    [[= Value("jwt.secret") ]]
    std::string jwt_secret = "default-secret";

    [[= Bean() ]] [[= Order(1) ]]
    std::shared_ptr<CorsMiddleware> cors_middleware() {
        return std::make_shared<CorsMiddleware>(
            CorsMiddleware::Config{
                .allowed_origins   = {"*"},
                .allowed_methods   = {"GET","POST","PUT","DELETE","PATCH","OPTIONS"},
                .allowed_headers   = {"Content-Type","Authorization"},
                .allow_credentials = false,
                .max_age_seconds   = 86400,
            });
    }

    [[= Bean() ]] [[= Order(2) ]]
    std::shared_ptr<SecurityHeadersMiddleware> security_headers() {
        return std::make_shared<SecurityHeadersMiddleware>();
    }

    [[= Bean() ]] [[= Order(3) ]]
    std::shared_ptr<JwtMiddleware> jwt_middleware() {
        return std::make_shared<JwtMiddleware>(
            JwtMiddleware::Config{
                .allowed_algorithms = { JwtMiddleware::Algorithm::HS256 },
                .hmac_secret = jwt_secret,
                .allowlist_paths = {"/", "/index.html", "/public/*"},
                .required_issuer = "novaboot-sample",
                .required_audiences = {"sample-api"},
                .required_scopes = {"read"},
            });
    }

    [[= Bean() ]] [[= Order(4) ]]
    std::shared_ptr<RequestLoggingMiddleware> request_logging() {
        return std::make_shared<RequestLoggingMiddleware>();
    }
};

} // namespace todo_notes::config
