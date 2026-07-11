#include <gtest/gtest.h>

#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/router/route.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/context/request_context.h"

using namespace novaboot;
using namespace novaboot::middleware;

// ── helper: run a single middleware in a one-element pipeline ─────────────────
// Handler is move_only_function — must be passed as rvalue.
static void run_one(Middleware& mw,
                    http3::Request& req,
                    http3::Response& res,
                    context::RequestContext& ctx,
                    router::Handler handler) {          // taken by value = moved in
    Pipeline p;
    p.add(std::shared_ptr<Middleware>(&mw, [](auto*){}));
    p.execute(req, res, ctx, handler);
}

// Default no-op handler factory (must be created fresh each call — move_only)
static router::Handler make_noop() {
    return [](http3::Request&, http3::Response& r, context::RequestContext&) {
        r.status(200).body("OK");
    };
}

// ══ CorsMiddleware ════════════════════════════════════════════════════════════

struct CorsTest : testing::Test {
    http3::Request           req;
    http3::Response          res;
    context::RequestContext  ctx;
};

TEST_F(CorsTest, DefaultConfigAddsWildcardOrigin) {
    CorsMiddleware mw;
    req.set_method("GET");
    req.set_path("/api/test");
    run_one(mw, req, res, ctx, make_noop());

    auto origin = res.headers().get("Access-Control-Allow-Origin");
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(*origin, "*");
}

TEST_F(CorsTest, AddsAllowMethodsHeader) {
    CorsMiddleware mw;
    req.set_method("GET");
    run_one(mw, req, res, ctx, make_noop());

    auto methods = res.headers().get("Access-Control-Allow-Methods");
    ASSERT_TRUE(methods.has_value());
    EXPECT_NE(methods->find("GET"),  std::string_view::npos);
    EXPECT_NE(methods->find("POST"), std::string_view::npos);
}

TEST_F(CorsTest, AddsAllowHeadersHeader) {
    CorsMiddleware mw;
    req.set_method("GET");
    run_one(mw, req, res, ctx, make_noop());

    auto hdrs = res.headers().get("Access-Control-Allow-Headers");
    ASSERT_TRUE(hdrs.has_value());
    EXPECT_NE(hdrs->find("Content-Type"), std::string_view::npos);
}

TEST_F(CorsTest, PreflightShortCircuits) {
    CorsMiddleware mw;
    req.set_method("OPTIONS");
    req.set_path("/api/test");

    bool handler_called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            handler_called = true;
        });

    EXPECT_FALSE(handler_called) << "OPTIONS must not reach the handler";
    EXPECT_EQ(res.status_code(), 204);
    auto max_age = res.headers().get("Access-Control-Max-Age");
    ASSERT_TRUE(max_age.has_value());
    EXPECT_EQ(*max_age, "86400");
}

TEST_F(CorsTest, NonPreflightCallsHandler) {
    CorsMiddleware mw;
    req.set_method("POST");

    bool handler_called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            handler_called = true;
            r.status(201);
        });

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(res.status_code(), 201);
}

TEST_F(CorsTest, AllowCredentialsTrueAddsHeader) {
    CorsMiddleware::Config cfg;
    cfg.allow_credentials = true;
    CorsMiddleware mw(cfg);
    req.set_method("GET");
    run_one(mw, req, res, ctx, make_noop());

    auto cred = res.headers().get("Access-Control-Allow-Credentials");
    ASSERT_TRUE(cred.has_value());
    EXPECT_EQ(*cred, "true");
}

TEST_F(CorsTest, AllowCredentialsFalseOmitsHeader) {
    CorsMiddleware mw;   // default: allow_credentials = false
    req.set_method("GET");
    run_one(mw, req, res, ctx, make_noop());

    EXPECT_FALSE(res.headers().get("Access-Control-Allow-Credentials").has_value());
}

TEST_F(CorsTest, SpecificOriginReflected) {
    CorsMiddleware::Config cfg;
    cfg.allowed_origins = {"https://example.com", "https://other.com"};
    CorsMiddleware mw(cfg);

    req.set_method("GET");
    req.headers().set("origin", "https://example.com");
    run_one(mw, req, res, ctx, make_noop());

    auto origin = res.headers().get("Access-Control-Allow-Origin");
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(*origin, "https://example.com");
}

// ══ RequestLoggingMiddleware ══════════════════════════════════════════════════

TEST(RequestLoggingTest, CallsNextAndPreservesStatusAndBody) {
    RequestLoggingMiddleware mw;
    http3::Request           req;
    http3::Response          res;
    context::RequestContext  ctx;
    req.set_method("GET");
    req.set_path("/api/users/1");

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(200).body("hello");
        });

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
    EXPECT_EQ(res.body_data(), "hello");
}

TEST(RequestLoggingTest, DoesNotInjectExtraHeaders) {
    RequestLoggingMiddleware mw;
    http3::Request           req;
    http3::Response          res;
    context::RequestContext  ctx;
    req.set_method("DELETE");
    req.set_path("/api/items/42");
    run_one(mw, req, res, ctx, make_noop());

    // Logging middleware must not inject any headers of its own
    EXPECT_FALSE(res.headers().get("X-Log").has_value());
}

// ══ Combined pipeline: CORS → Logging → handler ══════════════════════════════

TEST(CombinedPipelineTest, NormalRequestFlowsThrough) {
    Pipeline pipeline;
    pipeline.add(std::make_shared<CorsMiddleware>());
    pipeline.add(std::make_shared<RequestLoggingMiddleware>());

    http3::Request           req;
    http3::Response          res;
    context::RequestContext  ctx;
    req.set_method("GET");
    req.set_path("/api/ping");

    bool called = false;
    router::Handler handler =
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(200).json(R"({"pong":true})");
        };

    pipeline.execute(req, res, ctx, handler);

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
    EXPECT_TRUE(res.headers().get("Access-Control-Allow-Origin").has_value());
}

TEST(CombinedPipelineTest, PreflightBlocksHandler) {
    Pipeline pipeline;
    pipeline.add(std::make_shared<CorsMiddleware>());
    pipeline.add(std::make_shared<RequestLoggingMiddleware>());

    http3::Request           req;
    http3::Response          res;
    context::RequestContext  ctx;
    req.set_method("OPTIONS");
    req.set_path("/api/ping");

    bool called = false;
    router::Handler handler =
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            called = true;
        };

    pipeline.execute(req, res, ctx, handler);

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 204);
    EXPECT_TRUE(res.headers().get("Access-Control-Allow-Origin").has_value());
}
