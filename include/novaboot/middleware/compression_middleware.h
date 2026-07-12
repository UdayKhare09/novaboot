#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Compresses eligible responses with gzip when the client advertises support.
class CompressionMiddleware : public Middleware {
public:
    struct Config {
        std::size_t min_size_bytes = 1024;
        int gzip_level = 6;
        std::vector<std::string> compressible_content_types = {
            "text/",
            "application/json",
            "application/javascript",
            "application/xml",
            "application/problem+json",
        };
    };

    CompressionMiddleware();
    explicit CompressionMiddleware(Config cfg);

    void handle(http3::Request& req,
                http3::Response& res,
                context::RequestContext& ctx,
                Next next) override;

private:
    Config cfg_;
};

} // namespace novaboot::middleware
