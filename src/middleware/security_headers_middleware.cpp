#include "novaboot/middleware/security_headers_middleware.h"

#include <cctype>
#include <stdexcept>
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

bool token_character(unsigned char value) noexcept {
    return std::isalnum(value) || value == '!' || value == '#' || value == '$' ||
           value == '%' || value == '&' || value == '\'' || value == '*' ||
           value == '+' || value == '-' || value == '.' || value == '^' ||
           value == '_' || value == '`' || value == '|' || value == '~';
}

} // namespace

SecurityHeadersMiddleware::SecurityHeadersMiddleware()
    : SecurityHeadersMiddleware(Config{}) {}

SecurityHeadersMiddleware::SecurityHeadersMiddleware(Config cfg)
    : cfg_(std::move(cfg)) {
    if (cfg_.hsts) validate_header("Strict-Transport-Security", cfg_.hsts_value);
    if (cfg_.content_type_options) {
        validate_header("X-Content-Type-Options", cfg_.content_type_options_value);
    }
    if (cfg_.frame_options) validate_header("X-Frame-Options", cfg_.frame_options_value);
    if (cfg_.x_xss_protection) {
        validate_header("X-XSS-Protection", cfg_.x_xss_protection_value);
    }
    if (cfg_.referrer_policy) validate_header("Referrer-Policy", cfg_.referrer_policy_value);
    if (cfg_.permissions_policy) validate_header("Permissions-Policy", cfg_.permissions_policy_value);
    if (cfg_.cross_origin_opener_policy) {
        validate_header("Cross-Origin-Opener-Policy", cfg_.cross_origin_opener_policy_value);
    }
    if (cfg_.cross_origin_resource_policy) {
        validate_header("Cross-Origin-Resource-Policy", cfg_.cross_origin_resource_policy_value);
    }
    if (!cfg_.content_security_policy.empty()) {
        validate_header("Content-Security-Policy", cfg_.content_security_policy);
    }
    for (const auto& [name, value] : cfg_.custom_headers) validate_header(name, value);
}

void SecurityHeadersMiddleware::validate_header(std::string_view name,
                                                std::string_view value) {
    if (name.empty()) throw std::invalid_argument("Security header name must not be empty");
    for (const auto character : name) {
        if (!token_character(static_cast<unsigned char>(character))) {
            throw std::invalid_argument("Security header name contains an invalid character");
        }
    }
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7f) {
            throw std::invalid_argument("Security header value contains a control character");
        }
    }
}

void SecurityHeadersMiddleware::handle(http3::Request& req,
                                       http3::Response& res,
                                       context::RequestContext& ctx,
                                       Next next) {
    (void)ctx;

    next();

    if (cfg_.hsts && (!cfg_.hsts_https_only || req.scheme() == "https")) {
        set_if_absent(res, "Strict-Transport-Security", cfg_.hsts_value);
    }
    if (cfg_.content_type_options) {
        set_if_absent(res, "X-Content-Type-Options",
                      cfg_.content_type_options_value);
    }
    if (cfg_.frame_options) {
        set_if_absent(res, "X-Frame-Options", cfg_.frame_options_value);
    }
    if (cfg_.x_xss_protection) {
        set_if_absent(res, "X-XSS-Protection", cfg_.x_xss_protection_value);
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
