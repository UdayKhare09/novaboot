#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Rejects requests whose body is larger than the configured limit.
class BodySizeLimitMiddleware : public Middleware {
public:
    struct Config {
        std::size_t max_body_bytes = 1024 * 1024;
        std::vector<std::string> allowlist_paths = {};
        int status_code = 413;
        std::string response_body =
            R"({"error":"Payload Too Large","message":"Request body exceeds the configured limit"})";
    };

    BodySizeLimitMiddleware();
    explicit BodySizeLimitMiddleware(Config cfg);

    void handle(http3::Request& req,
                http3::Response& res,
                context::RequestContext& ctx,
                Next next) override;

private:
    Config cfg_;
};

} // namespace novaboot::middleware
