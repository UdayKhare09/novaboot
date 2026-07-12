#include "novaboot/middleware/security_headers_middleware.h"

#include <utility>

namespace novaboot::middleware {

namespace {

void set_if_absent(http3::Response& res,
                   std::string_view name,
                   std::string_view value) {
    if (!value.empty() && !res.headers().has(name)) {
        res.header(name, value);
    }
}

} // namespace

SecurityHeadersMiddleware::SecurityHeadersMiddleware()
    : SecurityHeadersMiddleware(Config{}) {}

SecurityHeadersMiddleware::SecurityHeadersMiddleware(Config cfg)
    : cfg_(std::move(cfg)) {}

void SecurityHeadersMiddleware::handle(http3::Request& req,
                                       http3::Response& res,
                                       context::RequestContext& ctx,
                                       Next next) {
    (void)req;
    (void)ctx;

    next();

    if (cfg_.hsts) {
        set_if_absent(res, "Strict-Transport-Security", cfg_.hsts_value);
    }
    if (cfg_.content_type_options) {
        set_if_absent(res, "X-Content-Type-Options",
                      cfg_.content_type_options_value);
    }
    if (cfg_.frame_options) {
        set_if_absent(res, "X-Frame-Options", cfg_.frame_options_value);
    }
    if (cfg_.referrer_policy) {
        set_if_absent(res, "Referrer-Policy", cfg_.referrer_policy_value);
    }
    if (cfg_.permissions_policy) {
        set_if_absent(res, "Permissions-Policy",
                      cfg_.permissions_policy_value);
    }
    if (cfg_.cross_origin_opener_policy) {
        set_if_absent(res, "Cross-Origin-Opener-Policy",
                      cfg_.cross_origin_opener_policy_value);
    }
    if (cfg_.cross_origin_resource_policy) {
        set_if_absent(res, "Cross-Origin-Resource-Policy",
                      cfg_.cross_origin_resource_policy_value);
    }

    set_if_absent(res, "Content-Security-Policy",
                  cfg_.content_security_policy);

    for (const auto& [name, value] : cfg_.custom_headers) {
        set_if_absent(res, name, value);
    }
}

} // namespace novaboot::middleware
