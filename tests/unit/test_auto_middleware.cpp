#include <gtest/gtest.h>
#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/annotations/scanner.h"
#include <filesystem>
#include <vector>
#include <string>

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;

// Global trace log for order verification
static std::vector<std::string> execution_log;

struct [[= Order(20) ]] MockMiddlewareA : public middleware::Middleware {
    void handle(http3::Request& req, http3::Response& res,
                context::RequestContext& ctx, Next next) override {
        execution_log.push_back("A");
        next();
    }
};

struct [[= Order(10) ]] MockMiddlewareB : public middleware::Middleware {
    void handle(http3::Request& req, http3::Response& res,
                context::RequestContext& ctx, Next next) override {
        execution_log.push_back("B");
        next();
    }
};

static std::string get_cert_path() {
    std::filesystem::path p = "cert.pem";
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(p)) return p.string();
        p = "../" / p;
    }
    return "";
}

static std::string get_key_path() {
    std::filesystem::path p = "key.pem";
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(p)) return p.string();
        p = "../" / p;
    }
    return "";
}

TEST(AutoMiddlewareTest, OrderAndAutoResolution) {
    execution_log.clear();

    RootContainer di_root;
    // Register both as singletons
    di_root.register_bean<MockMiddlewareA>([](ContainerBase&) { return new MockMiddlewareA(); });
    di_root.register_bean<MockMiddlewareB>([](ContainerBase&) { return new MockMiddlewareB(); });
    di_root.build();

    auto cert = get_cert_path();
    auto key = get_key_path();
    ASSERT_FALSE(cert.empty());
    ASSERT_FALSE(key.empty());

    // Setup Server using Server::Builder with the DI container
    auto server = Server::create()
        .tls(cert, key)
        .di_container(di_root)
        .build();

    // Verify the pipeline was constructed with middlewares in correct order (B then A)
    // Pipeline includes the built-in HTTP drain gate before DI and the ordered
    // application middlewares.
    EXPECT_EQ(server->pipeline().size(), 4);

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;

    auto shard_container = di_root.make_shard_container();
    shard_container->initialize();
    auto req_container = shard_container->make_request_container();
    ctx.bind_container(*req_container);

    router::Handler final_handler = [](http3::Request&, http3::Response&, context::RequestContext&) {
        execution_log.push_back("Handler");
    };

    server->pipeline().execute(req, res, ctx, final_handler);

    // Verify execution order: MockMiddlewareB (Order 10) -> MockMiddlewareA (Order 20) -> Handler
    ASSERT_EQ(execution_log.size(), 3);
    EXPECT_EQ(execution_log[0], "B");
    EXPECT_EQ(execution_log[1], "A");
    EXPECT_EQ(execution_log[2], "Handler");

    di_root.shutdown();
}
