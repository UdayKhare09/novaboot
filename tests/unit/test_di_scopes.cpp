/// @file test_di_scopes.cpp
/// Tests for Request and Connection scope containers.

#include <gtest/gtest.h>
#include "novaboot/di/container.h"

using namespace novaboot::di;

// ─── Test beans ───────────────────────────────────────────────────────────────

struct RequestLogger {
    static int instances;
    int id;
    RequestLogger() : id(++instances) {}
    ~RequestLogger() { --instances; }
};
int RequestLogger::instances = 0;

struct ConnectionRateLimiter {
    static int instances;
    int id;
    ConnectionRateLimiter() : id(++instances) {}
    ~ConnectionRateLimiter() { --instances; }
};
int ConnectionRateLimiter::instances = 0;

struct Singleton { int id = 42; };

// ─── Fixture ──────────────────────────────────────────────────────────────────

class DIScopeTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::make_unique<RootContainer>();
        root_->register_bean<Singleton>([](ContainerBase&) { return new Singleton{}; });
        root_->build();
        shard_ = root_->make_shard_container();
    }

    std::unique_ptr<RootContainer>   root_;
    std::unique_ptr<ShardContainer>  shard_;
};

// ─── Request Scope Tests ──────────────────────────────────────────────────────

TEST_F(DIScopeTest, RequestScopeSameInstanceWithinOneContainer) {
    auto req = shard_->make_request_container();
    req->register_bean<RequestLogger>([](ContainerBase&) { return new RequestLogger{}; });

    auto& l1 = req->resolve<RequestLogger>();
    auto& l2 = req->resolve<RequestLogger>();
    EXPECT_EQ(&l1, &l2) << "Same RequestLogger within same RequestContainer";
}

TEST_F(DIScopeTest, RequestScopeDifferentInstanceAcrossContainers) {
    auto factory = [](ContainerBase&) { return new RequestLogger{}; };

    auto req1 = shard_->make_request_container();
    req1->register_bean<RequestLogger>(factory);
    auto& l1 = req1->resolve<RequestLogger>();

    auto req2 = shard_->make_request_container();
    req2->register_bean<RequestLogger>(factory);
    auto& l2 = req2->resolve<RequestLogger>();

    EXPECT_NE(&l1, &l2) << "Different instances across different RequestContainers";
}

TEST_F(DIScopeTest, RequestScopeBeanDestroyedWhenContainerDropped) {
    RequestLogger::instances = 0;
    {
        auto req = shard_->make_request_container();
        req->register_bean<RequestLogger>([](ContainerBase&) { return new RequestLogger{}; });
        req->resolve<RequestLogger>();
        EXPECT_EQ(RequestLogger::instances, 1);
    }  // req destroyed here
    EXPECT_EQ(RequestLogger::instances, 0);
}

TEST_F(DIScopeTest, RequestScopeCanAccessSingleton) {
    auto req = shard_->make_request_container();
    // Singleton is in root — request scope delegates up via parent chain
    auto& s = req->resolve<Singleton>();
    EXPECT_EQ(s.id, 42);
}

// ─── Connection Scope Tests ───────────────────────────────────────────────────

TEST_F(DIScopeTest, ConnectionScopeSameInstanceWithinOneContainer) {
    auto conn = shard_->make_connection_container();
    conn->register_bean<ConnectionRateLimiter>([](ContainerBase&) {
        return new ConnectionRateLimiter{};
    });
    auto& r1 = conn->resolve<ConnectionRateLimiter>();
    auto& r2 = conn->resolve<ConnectionRateLimiter>();
    EXPECT_EQ(&r1, &r2);
}

TEST_F(DIScopeTest, ConnectionScopeDifferentInstanceAcrossConnections) {
    auto factory = [](ContainerBase&) { return new ConnectionRateLimiter{}; };

    auto conn1 = shard_->make_connection_container();
    conn1->register_bean<ConnectionRateLimiter>(factory);
    auto& r1 = conn1->resolve<ConnectionRateLimiter>();

    auto conn2 = shard_->make_connection_container();
    conn2->register_bean<ConnectionRateLimiter>(factory);
    auto& r2 = conn2->resolve<ConnectionRateLimiter>();

    EXPECT_NE(&r1, &r2);
}

TEST_F(DIScopeTest, ConnectionScopeBeanDestroyedWhenContainerDropped) {
    ConnectionRateLimiter::instances = 0;
    {
        auto conn = shard_->make_connection_container();
        conn->register_bean<ConnectionRateLimiter>([](ContainerBase&) {
            return new ConnectionRateLimiter{};
        });
        conn->resolve<ConnectionRateLimiter>();
        EXPECT_EQ(ConnectionRateLimiter::instances, 1);
    }
    EXPECT_EQ(ConnectionRateLimiter::instances, 0);
}
