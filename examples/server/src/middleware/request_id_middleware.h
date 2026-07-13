#pragma once

#include "novaboot/middleware/middleware.h"

/// Custom in-app middleware — stamps every request/response with a unique ID.
///
/// - Reads the incoming X-Request-Id header; if absent, generates a new one.
/// - Stores the ID in the RequestContext (string key "request_id") so that
///   downstream handlers and services can log it for distributed tracing.
/// - Echoes the ID back in the X-Request-Id response header.
using namespace novaboot;

class RequestIdMiddleware : public middleware::Middleware {
public:
    RequestIdMiddleware() = default;

    void handle(http3::Request&          req,
                http3::Response&         res,
                context::RequestContext& ctx,
                Next                     next) override;

private:
    /// Generate a simple pseudo-UUID v4 (no external deps needed).
    static std::string generate_id();
};

