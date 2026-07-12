#pragma once

#include <string>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Policy-based route authorization middleware.
///
/// Must run after JwtMiddleware for protected policies, because it authorizes
/// the JwtPrincipal stored in RequestContext by the authentication layer.
class AuthorizationMiddleware : public Middleware {
public:
    enum class MatchMode {
        Any,
        All,
    };

    struct Policy {
        /// Path pattern. Exact match by default; trailing '*' matches prefix.
        std::string path = "*";

        /// Empty means all methods.
        std::vector<std::string> methods = {};

        /// If false, this policy explicitly allows matching requests.
        bool require_authenticated = true;

        std::vector<std::string> required_scopes = {};
        MatchMode scope_match = MatchMode::All;

        std::vector<std::string> required_roles = {};
        MatchMode role_match = MatchMode::All;
    };

    struct Config {
        /// First matching policy wins. If no policy matches, request is allowed.
        std::vector<Policy> policies = {};

        /// Claim used to read roles from JwtPrincipal::claims.
        /// Supports string arrays and space-separated strings.
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
