/// @file test_di_prototype.cpp
/// Unit tests for Prototype scope: new instance per resolve_new<T>().

#include <gtest/gtest.h>
#include "novaboot/di/container.h"

using namespace novaboot::di;

struct Counter {
    static int instances;
    int id;
    Counter() : id(++instances) {}
    ~Counter() { --instances; }
};
int Counter::instances = 0;

struct Widget {
    Counter counter;
    Widget() = default;
};

TEST(DIPrototype, ResolveNewReturnsDistinctInstances) {
    RootContainer root;
    root.register_bean<Widget>(
        [](ContainerBase&) { return new Widget{}; },
        Scope::Prototype
    );
    root.build();

    auto w1 = root.resolve_new<Widget>();
    auto w2 = root.resolve_new<Widget>();
    auto w3 = root.resolve_new<Widget>();

    EXPECT_NE(w1.get(), w2.get());
    EXPECT_NE(w2.get(), w3.get());
    EXPECT_NE(w1.get(), w3.get());
}

TEST(DIPrototype, ResolveNewOnSingletonThrows) {
    RootContainer root;
    root.register_bean<Widget>(
        [](ContainerBase&) { return new Widget{}; },
        Scope::Singleton
    );
    root.build();

    EXPECT_THROW(root.resolve_new<Widget>(), DIError);
}

TEST(DIPrototype, PrototypeInstanceDestroyedWhenUniquePtrDropped) {
    Counter::instances = 0;
    RootContainer root;

    root.register_bean<Counter>(
        [](ContainerBase&) { return new Counter{}; },
        Scope::Prototype
    );
    root.build();

    EXPECT_EQ(Counter::instances, 0);
    {
        auto c1 = root.resolve_new<Counter>();
        auto c2 = root.resolve_new<Counter>();
        EXPECT_EQ(Counter::instances, 2);
    }
    // Both unique_ptrs dropped → instances destroyed
    EXPECT_EQ(Counter::instances, 0);
}
