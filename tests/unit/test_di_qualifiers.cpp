/// @file test_di_qualifiers.cpp
/// Tests for named qualifiers, type-tag qualifiers, and @primary selection.

#include <gtest/gtest.h>
#include "novaboot/di/container.h"

using namespace novaboot::di;

// ─── Interface + implementations ──────────────────────────────────────────────

struct Cache {
    virtual ~Cache() = default;
    virtual const char* name() const = 0;
};

struct RedisCache final : Cache {
    const char* name() const override { return "redis"; }
};




struct MemcachedCache final : Cache {
    const char* name() const override { return "memcached"; }
};

// ─── Named qualifier tests ────────────────────────────────────────────────────

TEST(DIQualifiers, NamedQualifierResolution) {
    RootContainer root;

    root.register_bean<RedisCache>(
        [](ContainerBase&) { return new RedisCache{}; },
        Scope::Singleton,
        "redis"
    );
    root.register_bean<MemcachedCache>(
        [](ContainerBase&) { return new MemcachedCache{}; },
        Scope::Singleton,
        "memcached"
    );

    root.build();

    auto& redis = root.resolve_named<RedisCache>("redis");
    EXPECT_STREQ(redis.name(), "redis");

    auto& mc = root.resolve_named<MemcachedCache>("memcached");
    EXPECT_STREQ(mc.name(), "memcached");
}

TEST(DIQualifiers, UnknownQualifierThrows) {
    RootContainer root;
    root.register_bean<RedisCache>(
        [](ContainerBase&) { return new RedisCache{}; },
        Scope::Singleton,
        "redis"
    );
    root.build();

    EXPECT_THROW(root.resolve_named<RedisCache>("nonexistent"), DIError);
}

// ─── Primary bean ─────────────────────────────────────────────────────────────

struct PrimaryBean {
    int value;
    explicit PrimaryBean(int v) : value(v) {}
};

TEST(DIQualifiers, PrimaryBeanRegisteredAsDefault) {
    RootContainer root;

    // Register a "primary" bean with qualifier="primary" explicitly
    // In the codegen path, [[=primary{}]] sets is_primary=true and the
    // container registration omits the qualifier so it becomes the default.
    root.register_bean<PrimaryBean>(
        [](ContainerBase&) { return new PrimaryBean{42}; },
        Scope::Singleton,
        /*qualifier=*/ "",
        /*is_primary=*/ true
    );

    root.build();

    auto& pb = root.resolve<PrimaryBean>();
    EXPECT_EQ(pb.value, 42);
}

// ─── Inject<T> wrappers ───────────────────────────────────────────────────────

#include "novaboot/di/inject.h"

TEST(DIQualifiers, LazyWrapperDeferred) {
    RootContainer root;
    int constructed = 0;

    root.register_bean<RedisCache>([&constructed](ContainerBase&) {
        ++constructed;
        return new RedisCache{};
    });
    root.build();

    EXPECT_EQ(constructed, 1); // Singleton built eagerly

    Lazy<RedisCache> lazy{root};
    EXPECT_FALSE(lazy.is_resolved());
    // First access triggers resolution
    auto& r = lazy.get();
    EXPECT_TRUE(lazy.is_resolved());
    EXPECT_STREQ(r.name(), "redis");
    EXPECT_EQ(constructed, 1); // Already cached from build
}

TEST(DIQualifiers, OptionalPresentAndAbsent) {
    RootContainer root;
    root.register_bean<RedisCache>([](ContainerBase&) { return new RedisCache{}; });
    root.build();

    Optional<RedisCache> present{root};
    EXPECT_TRUE(present.has_value());
    EXPECT_STREQ(present->name(), "redis");

    Optional<MemcachedCache> absent{root};
    EXPECT_FALSE(absent.has_value());
    EXPECT_EQ(absent.get(), nullptr);
}

TEST(DIQualifiers, BaseClassInterfaceResolution) {
    RootContainer root;
    root.register_component<RedisCache>();
    root.build();

    // Resolve as base class interface Cache
    auto& cache = root.resolve<Cache>();
    EXPECT_STREQ(cache.name(), "redis");
}
