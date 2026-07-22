#pragma once

#include <memory>

#include "novaboot/middleware/middleware.h"
#include "novaboot/observability/observation.h"

namespace novaboot::observability {

/// Captures standard OpenTelemetry HTTP server metrics and W3C trace context.
/// Route paths are intentionally not recorded until router template matching is
/// available to avoid accidental high-cardinality metrics.
class HttpObservationMiddleware final : public middleware::Middleware {
public:
    explicit HttpObservationMiddleware(std::shared_ptr<MeterRegistry> meters);

    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;

private:
    std::shared_ptr<MeterRegistry> meters_;
};

} // namespace novaboot::observability
