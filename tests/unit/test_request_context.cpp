/// @file test_request_context.cpp
/// Tests for RequestContext including DI integration.

#include <gtest/gtest.h>
#include "novaboot/context/request_context.h"
#include "novaboot/di/container.h"

using namespace novaboot;
using namespace novaboot::context;
using namespace novaboot::di;

// ─── Original API tests ───────────────────────────────────────────────────────

TEST(RequestContext, SetAndGetByType) {
    RequestContext ctx;
    ctx.set<int>(42);
    auto* val = ctx.get<int>();
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, 42);
}

TEST(RequestContext, GetMissingTypeReturnsNull) {
    RequestContext ctx;
    EXPECT_EQ(ctx.get<double>(), nullptr);
}

TEST(RequestContext, HasType) {
    RequestContext ctx;
    EXPECT_FALSE(ctx.has<int>());
    ctx.set<int>(1);
    EXPECT_TRUE(ctx.has<int>());
}

TEST(RequestContext, RemoveType) {
    RequestContext ctx;
    ctx.set<std::string>("hello");
    EXPECT_TRUE(ctx.has<std::string>());
    ctx.remove<std::string>();
    EXPECT_FALSE(ctx.has<std::string>());
}

TEST(RequestContext, SetGetString) {
    RequestContext ctx;
    ctx.set_string("user-id", "u123");
    EXPECT_EQ(ctx.get_string("user-id"), "u123");
    EXPECT_EQ(ctx.get_string("missing"), "");
}

TEST(RequestContext, Clear) {
    RequestContext ctx;
    ctx.set<int>(1);
    ctx.set_string("key", "val");
    ctx.clear();
    EXPECT_FALSE(ctx.has<int>());
    EXPECT_EQ(ctx.get_string("key"), "");
}

// ─── DI integration tests ─────────────────────────────────────────────────────

struct Service { int value = 99; };

TEST(RequestContext, InjectWithoutContainerThrows) {
    RequestContext ctx;
    EXPECT_THROW((void)ctx.inject<Service>(), std::runtime_error);
}

TEST(RequestContext, InjectWithContainerReturnsBean) {
    RootContainer root;
    root.register_bean<Service>([](ContainerBase&) { return new Service{}; });
    root.build();

    auto shard = root.make_shard_container();
    auto req   = shard->make_request_container();

    RequestContext ctx;
    ctx.bind_container(*req);

    auto& svc = ctx.inject<Service>();
    EXPECT_EQ(svc.value, 99);
}

TEST(RequestContext, ContainerPointerNullBeforeBind) {
    RequestContext ctx;
    EXPECT_EQ(ctx.di_container(), nullptr);
}

TEST(RequestContext, ContainerPointerSetAfterBind) {
    RootContainer root;
    root.build();

    auto shard = root.make_shard_container();
    auto req   = shard->make_request_container();

    RequestContext ctx;
    ctx.bind_container(*req);
    EXPECT_NE(ctx.di_container(), nullptr);
}
