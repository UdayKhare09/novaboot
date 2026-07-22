#include "novaboot/middleware/declarative_authorization.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/session_middleware.h"

namespace novaboot::middleware {
namespace {

std::vector<std::string_view> split_values(std::string_view values) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (begin < values.size()) {
        while (begin < values.size() &&
               (std::isspace(static_cast<unsigned char>(values[begin])) || values[begin] == ',')) {
            ++begin;
        }
        std::size_t end = begin;
        while (end < values.size() &&
               !std::isspace(static_cast<unsigned char>(values[end])) && values[end] != ',') {
            ++end;
        }
        if (end > begin) result.push_back(values.substr(begin, end - begin));
        begin = end;
    }
    return result;
}

bool contains(const std::vector<std::string>& actual, std::string_view required) {
    return std::ranges::any_of(actual, [required](const std::string& value) {
        return value == required;
    });
}

bool satisfies(const std::vector<std::string>& actual, std::string_view required,
               annotations::AuthorizationMatch match) {
    const auto values = split_values(required);
    if (values.empty()) return true;
    if (match == annotations::AuthorizationMatch::Any) {
        return std::ranges::any_of(values, [&](std::string_view value) {
            return contains(actual, value);
        });
    }
    return std::ranges::all_of(values, [&](std::string_view value) {
        return contains(actual, value);
    });
}

std::vector<std::string> jwt_roles(const JwtPrincipal& principal) {
    if (const auto* roles = principal.claims.string_array("roles")) return *roles;
    if (const auto role_text = principal.claims.string("roles")) {
        std::vector<std::string> roles;
        for (const auto role : split_values(*role_text)) roles.emplace_back(role);
        return roles;
    }
    return {};
}

} // namespace

DeclarativeAuthorizationDecision
evaluate_declarative(const context::RequestContext& context,
                     const DeclarativeAuthorization& requirements) {
    const auto* jwt = context.get<JwtPrincipal>();
    const auto* session = context.get<SessionPrincipal>();
    if (jwt == nullptr && session == nullptr) {
        return DeclarativeAuthorizationDecision::Unauthenticated;
    }

    std::vector<std::string> roles;
    const std::vector<std::string>* scopes = nullptr;
    if (jwt != nullptr) {
        roles = jwt_roles(*jwt);
        scopes = &jwt->scopes;
    } else {
        roles = session->roles;
        scopes = &session->scopes;
    }

    if (!satisfies(roles, requirements.roles, requirements.role_match) ||
        !satisfies(*scopes, requirements.scopes, requirements.scope_match)) {
        return DeclarativeAuthorizationDecision::Forbidden;
    }
    return DeclarativeAuthorizationDecision::Allowed;
}

bool authorize_declarative(http3::Request& /*request*/, http3::Response& response,
                           context::RequestContext& context,
                           const DeclarativeAuthorization& requirements) {
    switch (evaluate_declarative(context, requirements)) {
    case DeclarativeAuthorizationDecision::Allowed:
        return true;
    case DeclarativeAuthorizationDecision::Unauthenticated:
        response.status(401)
            .header("WWW-Authenticate", R"(Bearer error="invalid_token")")
            .json(R"({"error":"Unauthorized","message":"Authentication required"})");
        return false;
    case DeclarativeAuthorizationDecision::Forbidden:
        response.status(403)
            .json(R"({"error":"Forbidden","message":"Insufficient permissions"})");
        return false;
    }
    return false;
}

} // namespace novaboot::middleware
