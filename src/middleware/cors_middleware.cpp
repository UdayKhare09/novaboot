#include "novaboot/middleware/cors_middleware.h"

#include <numeric>

namespace novaboot::middleware {

// ─── helpers ─────────────────────────────────────────────────────────────────

std::string CorsMiddleware::join(const std::vector<std::string>& v,
                                  std::string_view sep) {
    if (v.empty()) return {};
    return std::accumulate(
        std::next(v.begin()), v.end(), v.front(),
        [&](std::string a, const std::string& b) {
            a += sep;
            a += b;
            return a;
        });
}

// ─── ctor ────────────────────────────────────────────────────────────────────

CorsMiddleware::CorsMiddleware()
    : CorsMiddleware(Config{}) {}

CorsMiddleware::CorsMiddleware(Config cfg)

    : cfg_(std::move(cfg))
    , allowed_origins_str_(join(cfg_.allowed_origins))
    , allowed_methods_str_(join(cfg_.allowed_methods))
    , allowed_headers_str_(join(cfg_.allowed_headers))
    , exposed_headers_str_(join(cfg_.exposed_headers))
{}

// ─── handle ──────────────────────────────────────────────────────────────────

void CorsMiddleware::handle(http3::Request&          req,
                             http3::Response&         res,
                             context::RequestContext& /*ctx*/,
                             Next                     next) {
    // Reflect the Origin back when a specific list is configured; otherwise
    // use the wildcard string built in the constructor.
    const auto origin_hdr = req.header("origin");
    std::string origin_val = allowed_origins_str_;

    if (origin_hdr && cfg_.allowed_origins.size() != 1 &&
        cfg_.allowed_origins[0] != "*") {
        for (const auto& o : cfg_.allowed_origins) {
            if (o == *origin_hdr) { origin_val = o; break; }
        }
    }

    res.header("Access-Control-Allow-Origin",  origin_val);
    res.header("Access-Control-Allow-Methods", allowed_methods_str_);
    res.header("Access-Control-Allow-Headers", allowed_headers_str_);

    if (!exposed_headers_str_.empty())
        res.header("Access-Control-Expose-Headers", exposed_headers_str_);

    if (cfg_.allow_credentials)
        res.header("Access-Control-Allow-Credentials", "true");

    // Preflight — short-circuit
    if (req.method() == "OPTIONS") {
        res.header("Access-Control-Max-Age",
                   std::to_string(cfg_.max_age_seconds));
        res.status(204);
        return; // do NOT call next()
    }

    next();
}

} // namespace novaboot::middleware
