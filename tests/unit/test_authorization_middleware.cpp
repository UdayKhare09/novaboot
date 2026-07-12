#include <gtest/gtest.h>

#include <memory>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/middleware/authorization_middleware.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/router/route.h"

using namespace novaboot;
using namespace novaboot::middleware;

namespace {

void run_one(Middleware& mw,
             http3::Request& req,
             http3::Response& res,
             context::RequestContext& ctx,
             router::Handler handler) {
    Pipeline pipeline;
    pipeline.add(std::shared_ptr<Middleware>(&mw, [](auto*) {}));
    pipeline.execute(req, res, ctx, handler);
}

JwtPrincipal principal(std::vector<std::string> scopes = {},
                       std::vector<std::string> roles = {}) {
    JwtPrincipal p;
    p.subject = "user-123";
    p.scopes = std::move(scopes);
    p.claims.string_array_claims["roles"] = std::move(roles);
    return p;
}

router::Handler ok_handler(bool& called) {
    return [&](http3::Request&, http3::Response& res, context::RequestContext&) {
        called = true;
        res.status(200).body("OK");
    };
}

} // namespace

TEST(AuthorizationMiddlewareTest, AllowsWhenNoPolicyMatches) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/admin/*",
            .required_roles = {"admin"},
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(AuthorizationMiddlewareTest, MissingPrincipalReturns401) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/api/*",
            .required_scopes = {"users:read"},
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 401);
    EXPECT_TRUE(res.headers().get("WWW-Authenticate").has_value());
}

TEST(AuthorizationMiddlewareTest, MissingScopeReturns403) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/api/*",
            .required_scopes = {"users:write"},
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    ctx.set<JwtPrincipal>(principal({"users:read"}));

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 403);
}

TEST(AuthorizationMiddlewareTest, RequiredScopeAllowsRequest) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/api/*",
            .required_scopes = {"users:read"},
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    ctx.set<JwtPrincipal>(principal({"users:read", "users:write"}));

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(AuthorizationMiddlewareTest, AnyScopeAllowsOneMatchingScope) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/api/*",
            .required_scopes = {"admin", "users:read"},
            .scope_match = AuthorizationMiddleware::MatchMode::Any,
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    ctx.set<JwtPrincipal>(principal({"users:read"}));

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(AuthorizationMiddlewareTest, RequiredRoleFromArrayClaimAllowsRequest) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/admin/*",
            .required_roles = {"admin"},
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/admin/users");
    ctx.set<JwtPrincipal>(principal({}, {"admin", "user"}));

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(AuthorizationMiddlewareTest, RoleFromSpaceSeparatedStringAllowsRequest) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/admin/*",
            .required_roles = {"admin"},
        }},
    });

    JwtPrincipal p;
    p.subject = "user-123";
    p.claims.string_claims["roles"] = "admin user";

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/admin/users");
    ctx.set<JwtPrincipal>(std::move(p));

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(AuthorizationMiddlewareTest, MethodSpecificPolicyOnlyAppliesToMethod) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/api/users",
            .methods = {"POST"},
            .required_scopes = {"users:write"},
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(AuthorizationMiddlewareTest, ExplicitAllowPolicyBypassesAuthentication) {
    AuthorizationMiddleware mw({
        .policies = {{
            .path = "/public/*",
            .require_authenticated = false,
        }},
    });

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/public/docs");

    bool called = false;
    run_one(mw, req, res, ctx, ok_handler(called));

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
}
