#pragma once

#include <string>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Cross-Origin Resource Sharing (CORS) middleware.
///
/// Adds the standard CORS response headers to every outgoing response.
/// For preflight (OPTIONS) requests it short-circuits the pipeline and
/// immediately responds with 204 No Content so the real handler is skipped.
///
/// Usage in main.cpp:
///   auto cors = std::make_shared<novaboot::middleware::CorsMiddleware>(
///       novaboot::middleware::CorsMiddleware::Config{
///           .allowed_origins = {"*"},
///           .allowed_methods = {"GET","POST","PUT","DELETE","PATCH","OPTIONS"},
///           .allowed_headers = {"Content-Type","Authorization"},
///           .allow_credentials = false,
///           .max_age_seconds  = 86400,
///       });
///   Server::create().middleware(cors)...
class CorsMiddleware : public Middleware {
public:
    struct Config {
        /// Origins to allow. Use {"*"} for open CORS.
        std::vector<std::string> allowed_origins   = {"*"};
        std::vector<std::string> allowed_methods   = {"GET","POST","PUT","DELETE","PATCH","OPTIONS"};
        std::vector<std::string> allowed_headers    = {"Content-Type","Authorization"};
        std::vector<std::string> exposed_headers    = {};
        bool                     allow_credentials  = false;
        int                      max_age_seconds    = 86400;
    };

    CorsMiddleware();  ///< Constructs with default Config (allow-all)
    explicit CorsMiddleware(Config cfg);


    void handle(http3::Request&       req,
                http3::Response&      res,
                context::RequestContext& ctx,
                Next                  next) override;

private:
    Config      cfg_;
    std::string allowed_origins_str_;
    std::string allowed_methods_str_;
    std::string allowed_headers_str_;
    std::string exposed_headers_str_;

    static std::string join(const std::vector<std::string>& v,
                            std::string_view sep = ", ");
};

} // namespace novaboot::middleware
