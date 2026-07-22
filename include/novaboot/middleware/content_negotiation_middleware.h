#pragma once

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Enforces the response representation selected by an endpoint against the
/// request Accept header. Routes remain responsible for choosing a supported
/// representation; this middleware returns 406 rather than silently sending an
/// incompatible one.
class ContentNegotiationMiddleware final : public Middleware {
public:
    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;
};

} // namespace novaboot::middleware
