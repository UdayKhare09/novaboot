#pragma once

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Request-logging middleware.
///
/// Logs each request as:
///   [novaboot] GET /api/users/1 → 200 (3ms)
///
/// Timing is measured wall-clock from when this middleware is entered to
/// when the downstream chain returns. The log is emitted via spdlog::info.
///
/// Usage:
///   Server::create()
///       .middleware(std::make_shared<novaboot::middleware::RequestLoggingMiddleware>())
///       ...
class RequestLoggingMiddleware : public Middleware {
public:
    RequestLoggingMiddleware() = default;

    void handle(http3::Request&          req,
                http3::Response&         res,
                context::RequestContext& ctx,
                Next                     next) override;
};

} // namespace novaboot::middleware
