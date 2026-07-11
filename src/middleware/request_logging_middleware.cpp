#include "novaboot/middleware/request_logging_middleware.h"

#include <chrono>
#include <spdlog/spdlog.h>

namespace novaboot::middleware {

void RequestLoggingMiddleware::handle(http3::Request&          req,
                                      http3::Response&         res,
                                      context::RequestContext& /*ctx*/,
                                      Next                     next) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();

    next();

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - t0).count();

    spdlog::info("[novaboot] {} {} → {} ({}ms)",
                 req.method(), req.path(),
                 res.status_code(), elapsed_ms);
}

} // namespace novaboot::middleware
