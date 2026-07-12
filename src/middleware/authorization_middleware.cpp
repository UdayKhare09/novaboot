#include "novaboot/middleware/authorization_middleware.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

#include "novaboot/middleware/jwt_middleware.h"

namespace novaboot::middleware {
namespace {

bool path_matches(std::string_view pattern, std::string_view path) {
    if (pattern == "*") return true;
    if (pattern.ends_with('*')) {
        pattern.remove_suffix(1);
        return path.starts_with(pattern);
    }
    return path == pattern;
}

std::string upper(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return result;
}

bool method_matches(const std::vector<std::string>& methods,
                    std::string_view request_method) {
    if (methods.empty()) return true;

    const auto normalized_request = upper(request_method);
    return std::ranges::any_of(methods, [&](const std::string& method) {
        return upper(method) == normalized_request;
    });
}

const AuthorizationMiddleware::Policy*
matching_policy(const AuthorizationMiddleware::Config& cfg,
                http3::Request& req) {
    for (const auto& policy : cfg.policies) {
        if (path_matches(policy.path, req.path()) &&
            method_matches(policy.methods, req.method())) {
            return &policy;
        }
    }
    return nullptr;
}

std::vector<std::string> split_space_separated(std::string_view text) {
    std::vector<std::string> values;
    std::size_t start = 0;

    while (start < text.size()) {
        while (start < text.size() &&
               std::isspace(static_cast<unsigned char>(text[start]))) {
            ++start;
        }

        auto end = start;
        while (end < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[end]))) {
            ++end;
        }

        if (end > start) values.emplace_back(text.substr(start, end - start));
        start = end;
    }

    return values;
}

std::vector<std::string> roles_from_claim(const JwtPrincipal& principal,
                                          std::string_view roles_claim) {
    if (const auto* roles = principal.claims.string_array(roles_claim)) {
        return *roles;
    }

    if (const auto roles = principal.claims.string(roles_claim)) {
        return split_space_separated(*roles);
    }

    return {};
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
    return std::ranges::any_of(values, [needle](const std::string& value) {
        return value == needle;
    });
}

bool satisfies(const std::vector<std::string>& actual,
               const std::vector<std::string>& required,
               AuthorizationMiddleware::MatchMode mode) {
    if (required.empty()) return true;

    if (mode == AuthorizationMiddleware::MatchMode::Any) {
        return std::ranges::any_of(required, [&](const std::string& value) {
            return contains(actual, value);
        });
    }

    return std::ranges::all_of(required, [&](const std::string& value) {
        return contains(actual, value);
    });
}

void reject_unauthorized(http3::Response& res,
                         const AuthorizationMiddleware::Config& cfg) {
    res.status(cfg.unauthorized_status)
        .header("WWW-Authenticate", R"(Bearer error="invalid_token")")
        .json(cfg.unauthorized_body);
}

void reject_forbidden(http3::Response& res,
                      const AuthorizationMiddleware::Config& cfg) {
    res.status(cfg.forbidden_status).json(cfg.forbidden_body);
}

} // namespace

AuthorizationMiddleware::AuthorizationMiddleware()
    : AuthorizationMiddleware(Config{}) {}

AuthorizationMiddleware::AuthorizationMiddleware(Config cfg)
    : cfg_(std::move(cfg)) {}

void AuthorizationMiddleware::handle(http3::Request& req,
                                     http3::Response& res,
                                     context::RequestContext& ctx,
                                     Next next) {
    const auto* policy = matching_policy(cfg_, req);
    if (policy == nullptr || !policy->require_authenticated) {
        next();
        return;
    }

    const auto* principal = ctx.get<JwtPrincipal>();
    if (principal == nullptr) {
        reject_unauthorized(res, cfg_);
        return;
    }

    if (!satisfies(principal->scopes,
                   policy->required_scopes,
                   policy->scope_match)) {
        reject_forbidden(res, cfg_);
        return;
    }

    const auto roles = roles_from_claim(*principal, cfg_.roles_claim);
    if (!satisfies(roles, policy->required_roles, policy->role_match)) {
        reject_forbidden(res, cfg_);
        return;
    }

    next();
}

} // namespace novaboot::middleware
