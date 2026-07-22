#include "novaboot/middleware/authorization_middleware.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/session_middleware.h"

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

bool policy_matches(const AuthorizationMiddleware::Policy& policy,
                    http3::Request& req) {
    return path_matches(policy.path, req.path()) &&
           method_matches(
               policy.method.empty() ? policy.methods : policy.method,
               req.method());
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
    bool matched_policy = false;
    bool requires_principal = false;

    for (const auto& policy : cfg_.policies) {
        if (!policy_matches(policy, req)) continue;
        matched_policy = true;
        if (policy.require_authenticated) {
            requires_principal = true;
            break;
        }
    }

    if (!matched_policy) {
        next();
        return;
    }

    const auto* jwt_principal = ctx.get<JwtPrincipal>();
    const auto* session_principal = ctx.get<SessionPrincipal>();
    const bool authenticated = jwt_principal != nullptr || session_principal != nullptr;
    if (requires_principal && !authenticated) {
        reject_unauthorized(res, cfg_);
        return;
    }

    std::vector<std::string> jwt_roles;
    const std::vector<std::string>* scopes = nullptr;
    const std::vector<std::string>* roles = nullptr;
    if (jwt_principal != nullptr) {
        jwt_roles = roles_from_claim(*jwt_principal, cfg_.roles_claim);
        scopes = &jwt_principal->scopes;
        roles = &jwt_roles;
    } else if (session_principal != nullptr) {
        scopes = &session_principal->scopes;
        roles = &session_principal->roles;
    }

    for (const auto& policy : cfg_.policies) {
        if (!policy_matches(policy, req)) {
            continue;
        }

        if (policy.require_authenticated) {
            if (!satisfies(*scopes,
                           policy.required_scopes,
                           policy.scope_match)) {
                reject_forbidden(res, cfg_);
                return;
            }

            if (!satisfies(*roles, policy.required_roles, policy.role_match)) {
                reject_forbidden(res, cfg_);
                return;
            }
        }

        for (const auto& custom_policy : policy.custom_policies) {
            if (!custom_policy(req, ctx)) {
                reject_forbidden(res, cfg_);
                return;
            }
        }
    }

    next();
}

} // namespace novaboot::middleware
