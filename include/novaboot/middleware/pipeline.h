#pragma once

#include <memory>
#include <vector>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/router/route.h"

namespace novaboot::middleware {

/// Middleware pipeline executor.
///
/// Composes a chain of middleware + final handler at server startup
/// (not per-request). Execution builds a recursive next() chain.
///
/// Thread-safety: READ-ONLY after construction. Safe to share.
class Pipeline {
public:
    Pipeline() = default;

    /// Add a global middleware (applied to all routes)
    void add(std::shared_ptr<Middleware> mw);

    /// Execute the middleware chain with a final handler
    void execute(http3::Request& req,
                 http3::Response& res,
                 context::RequestContext& ctx,
                 router::Handler& handler) const;

    /// Number of middleware in the pipeline
    [[nodiscard]] std::size_t size() const noexcept {
        return middlewares_.size();
    }

private:
    std::vector<std::shared_ptr<Middleware>> middlewares_;
};

} // namespace novaboot::middleware
