/// @file test_di_singleton.cpp
/// Unit tests for Singleton scope: one instance, post_construct called once.

#include <gtest/gtest.h>
#include "novaboot/di/container.h"
#include "novaboot/di/lifecycle.h"
#include "novaboot/di/attributes.h"

using namespace novaboot::di;

// ─── Test beans ───────────────────────────────────────────────────────────────

struct Logger {
    int post_construct_calls = 0;
    int pre_destroy_calls    = 0;
    void init()    { ++post_construct_calls; }
    void shutdown(){ ++pre_destroy_calls; }
};

struct Database {
    Logger& logger_;
    std::string url;
    explicit Database(Logger& logger, std::string url_)
        : logger_(logger), url(std::move(url_)) {}
};

struct UserService {
    Database& db_;
    explicit UserService(Database& db) : db_(db) {}
};

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST(DISingleton, SameInstanceEveryResolve) {
    RootContainer root;

    root.register_bean<Logger>([](ContainerBase&) { return new Logger{}; });
    root.register_bean<Database>([](ContainerBase& c) {
        return new Database{c.resolve<Logger>(), "postgres://localhost/test"};
    });
    root.register_bean<UserService>([](ContainerBase& c) {
        return new UserService{c.resolve<Database>()};
    });

    root.build();

    auto& svc1 = root.resolve<UserService>();
    auto& svc2 = root.resolve<UserService>();
    EXPECT_EQ(&svc1, &svc2) << "Singleton must return same instance";

    auto& db1 = root.resolve<Database>();
    auto& db2 = root.resolve<Database>();
    EXPECT_EQ(&db1, &db2);
}

TEST(DISingleton, DepsResolvedTransitively) {
    RootContainer root;

    root.register_bean<Logger>([](ContainerBase&) { return new Logger{}; });
    root.register_bean<Database>([](ContainerBase& c) {
        return new Database{c.resolve<Logger>(), "postgres://localhost/test"};
    });
    root.register_bean<UserService>([](ContainerBase& c) {
        return new UserService{c.resolve<Database>()};
    });

    root.build();

    auto& svc = root.resolve<UserService>();
    auto& db  = root.resolve<Database>();
    EXPECT_EQ(&svc.db_, &db) << "UserService must hold the same Database singleton";

    auto& logger = root.resolve<Logger>();
    EXPECT_EQ(&db.logger_, &logger) << "Database must hold the same Logger singleton";
}

TEST(DISingleton, PostConstructCalledOnce) {
    RootContainer root;

    root.register_bean<Logger>([](ContainerBase&) { return new Logger{}; });
    root.with_post_construct<Logger>([](Logger& l) { l.init(); });

    root.build();

    auto& logger = root.resolve<Logger>();
    EXPECT_EQ(logger.post_construct_calls, 1) << "post_construct must be called exactly once";

    // Resolve again — no extra calls
    root.resolve<Logger>();
    EXPECT_EQ(logger.post_construct_calls, 1);
}

TEST(DISingleton, PreDestroyCalledOnShutdown) {
    int pre_destroy_call_count = 0;  // side-channel, alive after Logger is deleted
    RootContainer root;

    root.register_bean<Logger>([](ContainerBase&) { return new Logger{}; });
    root.with_pre_destroy<Logger>([&pre_destroy_call_count](Logger&) {
        ++pre_destroy_call_count;
    });

    root.build();
    EXPECT_EQ(pre_destroy_call_count, 0);

    root.shutdown();
    EXPECT_EQ(pre_destroy_call_count, 1)
        << "pre_destroy must be called exactly once on shutdown";
}

TEST(DISingleton, BuildTwiceThrows) {
    RootContainer root;
    root.register_bean<Logger>([](ContainerBase&) { return new Logger{}; });
    root.build();
    EXPECT_THROW(root.build(), DIError);
}

TEST(DISingleton, UnregisteredBeanThrows) {
    RootContainer root;
    root.build();
    EXPECT_THROW(root.resolve<Logger>(), DIError);
}
