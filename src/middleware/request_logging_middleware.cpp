#include "novaboot/middleware/request_logging_middleware.h"

#include <chrono>
#include <spdlog/spdlog.h>

namespace novaboot::middleware {

void RequestLoggingMiddleware::handle(http3::Request&          req,
                                      http3::Response&         res,
                                      context::RequestContext& ctx,
                                      Next                     next) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();

    next();

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - t0).count();

    const auto request_id = ctx.get_string("request_id");
    const auto trace_id = ctx.get_string("trace_id");
    if (!request_id.empty() || !trace_id.empty()) {
        spdlog::info("[novaboot] request_id={} trace_id={} {} {} → {} ({}ms)",
                     request_id, trace_id, req.method(), req.path(),
                     res.status_code(), elapsed_ms);
    } else {
        spdlog::info("[novaboot] {} {} → {} ({}ms)",
                     req.method(), req.path(), res.status_code(), elapsed_ms);
    }
}

} // namespace novaboot::middleware
