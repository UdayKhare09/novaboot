/// @file test_di_handler_injection.cpp
/// Tests for handler-level DI injection via RequestContext.

#include <gtest/gtest.h>
#include "novaboot/di/container.h"
#include "novaboot/di/inject.h"
#include "novaboot/context/request_context.h"

using namespace novaboot;
using namespace novaboot::di;

// ─── Test services ────────────────────────────────────────────────────────────

struct UserRepository {
    std::string find_by_id(int id) {
        return "User-" + std::to_string(id);
    }
};

struct AuthService {
    bool is_authenticated = true;
};

struct RequestScopedLogger {
    std::vector<std::string> log;
    void record(const std::string& msg) { log.push_back(msg); }
};

// ─── Fixture ──────────────────────────────────────────────────────────────────

class DIHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::make_unique<RootContainer>();
        root_->register_bean<UserRepository>([](ContainerBase&) {
            return new UserRepository{};
        });
        root_->register_bean<AuthService>([](ContainerBase&) {
            return new AuthService{};
        });
        root_->build();
        shard_ = root_->make_shard_container();
    }

    std::unique_ptr<RootContainer>   root_;
    std::unique_ptr<ShardContainer>  shard_;
};

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST_F(DIHandlerTest, ExplicitInjectFromRequestContext) {
    auto req = shard_->make_request_container();
    req->register_bean<RequestScopedLogger>([](ContainerBase&) {
        return new RequestScopedLogger{};
    });

    context::RequestContext ctx;
    ctx.bind_container(*req);

    auto& repo   = ctx.inject<UserRepository>();
    auto& auth   = ctx.inject<AuthService>();
    auto& logger = ctx.inject<RequestScopedLogger>();

    EXPECT_EQ(repo.find_by_id(1), "User-1");
    EXPECT_TRUE(auth.is_authenticated);

    logger.record("request started");
    EXPECT_EQ(logger.log.size(), 1u);
    EXPECT_EQ(logger.log[0], "request started");
}

TEST_F(DIHandlerTest, InjectWithoutBoundContainerThrows) {
    context::RequestContext ctx;  // No container bound!
    EXPECT_THROW(ctx.inject<UserRepository>(), std::runtime_error);
}

TEST_F(DIHandlerTest, InjectSingletonFromRequestScope) {
    auto req = shard_->make_request_container();
    context::RequestContext ctx;
    ctx.bind_container(*req);

    auto& repo1 = ctx.inject<UserRepository>();
    auto& repo2 = ctx.inject<UserRepository>();
    EXPECT_EQ(&repo1, &repo2) << "Singleton injected via ctx.inject<T>() must be same instance";
}

TEST_F(DIHandlerTest, RequestScopedBeanIsolatedPerRequest) {
    auto req1 = shard_->make_request_container();
    auto req2 = shard_->make_request_container();

    auto factory = [](ContainerBase&) { return new RequestScopedLogger{}; };
    req1->register_bean<RequestScopedLogger>(factory);
    req2->register_bean<RequestScopedLogger>(factory);

    context::RequestContext ctx1, ctx2;
    ctx1.bind_container(*req1);
    ctx2.bind_container(*req2);

    auto& l1 = ctx1.inject<RequestScopedLogger>();
    auto& l2 = ctx2.inject<RequestScopedLogger>();
    EXPECT_NE(&l1, &l2) << "Request-scoped loggers must be separate instances";

    l1.record("req1-event");
    EXPECT_EQ(l1.log.size(), 1u);
    EXPECT_EQ(l2.log.size(), 0u) << "Loggers must be isolated";
}
