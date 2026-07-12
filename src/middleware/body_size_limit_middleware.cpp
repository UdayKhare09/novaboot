#include "novaboot/middleware/body_size_limit_middleware.h"

#include <optional>
#include <string>
#include <utility>

namespace novaboot::middleware {

namespace {

bool path_allowed(std::string_view path,
                  const std::vector<std::string>& allowlist) {
    for (const auto& pattern : allowlist) {
        if (pattern.ends_with('*')) {
            const std::string_view prefix(pattern.data(), pattern.size() - 1);
            if (path.starts_with(prefix)) return true;
        } else if (path == pattern) {
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> declared_content_length(http3::Request& req) {
    const auto header = req.header("content-length");
    if (!header) return std::nullopt;

    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(std::string(*header), &consumed);
        if (consumed != header->size()) return std::nullopt;
        return static_cast<std::size_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

BodySizeLimitMiddleware::BodySizeLimitMiddleware()
    : BodySizeLimitMiddleware(Config{}) {}

BodySizeLimitMiddleware::BodySizeLimitMiddleware(Config cfg)
    : cfg_(std::move(cfg)) {}

void BodySizeLimitMiddleware::handle(http3::Request& req,
                                     http3::Response& res,
                                     context::RequestContext& ctx,
                                     Next next) {
    (void)ctx;

    if (path_allowed(req.path(), cfg_.allowlist_paths)) {
        next();
        return;
    }

    const auto declared = declared_content_length(req);
    const auto effective_size = declared.value_or(req.content_length());

    if (effective_size > cfg_.max_body_bytes) {
        res.status(cfg_.status_code).json(cfg_.response_body);
        return;
    }

    next();
}

} // namespace novaboot::middleware
