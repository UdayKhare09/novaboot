#pragma once

#include <functional>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::middleware {

/// Middleware interface.
///
/// Each middleware receives the request, response, context, and a
/// `next` function to call the next middleware in the chain.
/// Middleware can:
///   - Modify the request before passing downstream
///   - Short-circuit the pipeline by NOT calling next()
///   - Modify the response after downstream returns
///
/// Example:
///   class LoggingMiddleware : public Middleware {
///   public:
///       void handle(Request& req, Response& res,
///                   RequestContext& ctx, Next next) override {
///           auto start = Clock::now();
///           next();  // Call downstream
///           auto elapsed = Clock::now() - start;
///           spdlog::info("{} {} → {} ({}ms)",
///                        req.method(), req.path(),
///                        res.status_code(), elapsed.count());
///       }
///   };
class Middleware {
public:
    using Next = std::move_only_function<void()>;

    virtual ~Middleware() = default;

    virtual void handle(http3::Request& req,
                        http3::Response& res,
                        context::RequestContext& ctx,
                        Next next) = 0;
};

} // namespace novaboot::middleware
