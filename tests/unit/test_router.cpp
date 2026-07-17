#include <gtest/gtest.h>
#include "novaboot/router/router.h"
#include <string>

using namespace novaboot::router;

TEST(RouterTest, StaticRoutes) {
    Router router;
    bool root_called = false;
    bool hello_called = false;

    router.add_route(Method::GET, "/", [&](auto&, auto&, auto&) {
        root_called = true;
    });

    router.add_route(Method::GET, "/api/hello", [&](auto&, auto&, auto&) {
        hello_called = true;
    });

    // Match root
    auto match1 = router.match(Method::GET, "/");
    ASSERT_NE(match1.handler, nullptr);
    novaboot::http3::Request req;
    novaboot::http3::Response res;
    novaboot::context::RequestContext ctx;
    (*match1.handler)(req, res, ctx);
    EXPECT_TRUE(root_called);

    // Match /api/hello
    auto match2 = router.match(Method::GET, "/api/hello");
    ASSERT_NE(match2.handler, nullptr);
    (*match2.handler)(req, res, ctx);
    EXPECT_TRUE(hello_called);

    // 404 Case
    auto match3 = router.match(Method::GET, "/nonexistent");
    EXPECT_EQ(match3.handler, nullptr);

    // Wrong method
    auto match4 = router.match(Method::POST, "/");
    EXPECT_EQ(match4.handler, nullptr);
}

TEST(RouterTest, ParamRoutes) {
    Router router;
    std::string matched_id;
    std::string matched_post_id;

    router.add_route(Method::GET, "/api/users/:id", [&](auto& req, auto&, auto&) {
        matched_id = req.path_params().get("id").value_or("");
    });

    router.add_route(Method::GET, "/api/users/:id/posts/:post_id", [&](auto& req, auto&, auto&) {
        matched_id = req.path_params().get("id").value_or("");
        matched_post_id = req.path_params().get("post_id").value_or("");
    });

    // Match single param
    auto match1 = router.match(Method::GET, "/api/users/42");
    ASSERT_NE(match1.handler, nullptr);
    novaboot::http3::Request req;
    req.path_params() = std::move(match1.params);
    novaboot::http3::Response res;
    novaboot::context::RequestContext ctx;
    (*match1.handler)(req, res, ctx);
    EXPECT_EQ(matched_id, "42");

    // Match dual param
    auto match2 = router.match(Method::GET, "/api/users/100/posts/200");
    ASSERT_NE(match2.handler, nullptr);
    req.path_params() = std::move(match2.params);
    (*match2.handler)(req, res, ctx);
    EXPECT_EQ(matched_id, "100");
    EXPECT_EQ(matched_post_id, "200");
}

TEST(RouterTest, IgnoresQueryStringWhenMatchingRoutes) {
    Router router;
    bool called = false;

    router.add_route(Method::GET, "/api/articles", [&](auto&, auto&, auto&) {
        called = true;
    });

    auto match = router.match(Method::GET, "/api/articles?page=0&size=10");
    ASSERT_NE(match.handler, nullptr);

    novaboot::http3::Request req;
    novaboot::http3::Response res;
    novaboot::context::RequestContext ctx;
    (*match.handler)(req, res, ctx);
    EXPECT_TRUE(called);

    auto missing = router.match(Method::GET, "/api/missing?page=0");
    EXPECT_EQ(missing.handler, nullptr);
}

TEST(RouterTest, WildcardRoutes) {
    Router router;
    std::string matched_path;

    router.add_route(Method::GET, "/static/*filepath", [&](auto& req, auto&, auto&) {
        matched_path = req.path_params().get("filepath").value_or("");
    });

    auto match = router.match(Method::GET, "/static/css/main.css");
    ASSERT_NE(match.handler, nullptr);
    novaboot::http3::Request req;
    req.path_params() = std::move(match.params);
    novaboot::http3::Response res;
    novaboot::context::RequestContext ctx;
    (*match.handler)(req, res, ctx);
    EXPECT_EQ(matched_path, "css/main.css");
}

TEST(RouterTest, MethodAny) {
    Router router;
    int called_count = 0;

    router.add_route(Method::ANY, "/api/health", [&](auto&, auto&, auto&) {
        called_count++;
    });

    auto match_get = router.match(Method::GET, "/api/health");
    ASSERT_NE(match_get.handler, nullptr);

    auto match_post = router.match(Method::POST, "/api/health");
    ASSERT_NE(match_post.handler, nullptr);
}

TEST(RouterTest, ExposesRouteMetadataForDiagnostics) {
    Router router;
    router.add_route(Method::GET, "/api/diagnostics", [](auto&, auto&, auto&) {});
    router.add_route(Method::POST, "/api/diagnostics", [](auto&, auto&, auto&) {});

    ASSERT_EQ(router.routes().size(), 2);
    EXPECT_EQ(router.routes()[0].method, Method::GET);
    EXPECT_EQ(router.routes()[0].pattern, "/api/diagnostics");
    EXPECT_EQ(router.routes()[1].method, Method::POST);
    EXPECT_EQ(router.routes()[1].pattern, "/api/diagnostics");
}
