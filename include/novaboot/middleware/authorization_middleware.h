#pragma once

#include <functional>
#include <string>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Policy-based route authorization middleware.
///
/// Must run after JwtMiddleware or SessionMiddleware for protected policies,
/// because it authorizes the principal stored in RequestContext by the
/// authentication layer.
class AuthorizationMiddleware : public Middleware {
public:
    /// Domain-specific authorization predicate. Return false to produce the
    /// configured forbidden response. The request context exposes a verified
    /// JwtPrincipal or SessionPrincipal when authentication is required.
    using CustomPolicy = std::function<bool(
        const http3::Request&, const context::RequestContext&)>;

    enum class MatchMode {
        Any,
        All,
    };

    struct Policy {
        /// Path pattern. Exact match by default; trailing '*' matches prefix.
        std::string path = "*";

        /// Empty means all methods. `method` is the preferred field; `methods`
        /// is kept as a compatibility alias.
        std::vector<std::string> method = {};
        std::vector<std::string> methods = {};

        /// If false, this policy explicitly allows matching requests.
        bool require_authenticated = true;

        std::vector<std::string> required_scopes = {};
        MatchMode scope_match = MatchMode::All;

        std::vector<std::string> required_roles = {};
        MatchMode role_match = MatchMode::All;

        /// All custom policies must allow the matching request. Custom policies
        /// also run for public (`require_authenticated = false`) policies,
        /// enabling explicit request-level checks without a principal.
        std::vector<CustomPolicy> custom_policies = {};
    };

    struct Config {
        /// All matching policies are enforced. If no policy matches, request is
        /// allowed.
        std::vector<Policy> policies = {};

        /// Claim used to read JWT roles from JwtPrincipal::claims. Session
        /// principals carry roles directly. Supports string arrays and
        /// space-separated JWT strings.
        std::string roles_claim = "roles";

        int unauthorized_status = 401;
        std::string unauthorized_body =
            R"({"error":"Unauthorized","message":"Authentication required"})";

        int forbidden_status = 403;
        std::string forbidden_body =
            R"({"error":"Forbidden","message":"Insufficient permissions"})";
    };

    AuthorizationMiddleware();
    explicit AuthorizationMiddleware(Config cfg);

    void handle(http3::Request& req,
                http3::Response& res,
                context::RequestContext& ctx,
                Next next) override;

private:
    Config cfg_;
};

} // namespace novaboot::middleware
