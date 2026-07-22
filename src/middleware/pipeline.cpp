#include "novaboot/middleware/pipeline.h"

namespace novaboot::middleware {

void Pipeline::add(std::shared_ptr<Middleware> mw) {
    middlewares_.push_back(std::move(mw));
}

void Pipeline::execute(http3::Request& req,
                       http3::Response& res,
                       context::RequestContext& ctx,
                       router::Handler& handler) const {

    context::RequestContext::Scope request_scope(ctx);

    if (middlewares_.empty()) {
        // No middleware — call handler directly
        handler(req, res, ctx);
        return;
    }

    // Build the chain from back to front.
    // The innermost function calls the route handler.
    // Each middleware wraps the next one.

    // Start with the final handler as the innermost "next"
    std::function<void()> chain = [&]() {
        handler(req, res, ctx);
    };

    // Wrap in reverse order so the first middleware runs first
    for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
        auto& mw = *it;
        chain = [&req, &res, &ctx, &mw,
                 next = std::move(chain)]() mutable {
            mw->handle(req, res, ctx, std::move(next));
        };
    }

    // Execute the chain
    chain();
}

} // namespace novaboot::middleware
