/// @file test_di_lifecycle.cpp
/// Tests for post_construct and pre_destroy lifecycle callbacks.

#include <gtest/gtest.h>
#include "novaboot/di/container.h"
#include "novaboot/di/lifecycle.h"

using namespace novaboot::di;

// ─── Test bean: interface-based lifecycle ─────────────────────────────────────

// Shared log so it remains valid after the LifecycleService is destroyed
static std::vector<std::string> g_lifecycle_log;

struct LifecycleService : public Initializable, public Destroyable {
    void post_construct() override { g_lifecycle_log.push_back("post_construct"); }
    void pre_destroy()    override { g_lifecycle_log.push_back("pre_destroy"); }
};

TEST(DILifecycle, InterfaceBasedLifecycle) {
    g_lifecycle_log.clear();

    RootContainer root;
    root.register_bean<LifecycleService>([](ContainerBase&) {
        return new LifecycleService{};
    });
    root.build();

    // post_construct fired at build()
    ASSERT_EQ(g_lifecycle_log.size(), 1u);
    EXPECT_EQ(g_lifecycle_log[0], "post_construct");

    root.shutdown();  // ← LifecycleService is deleted here

    // pre_destroy must have fired before deletion
    ASSERT_EQ(g_lifecycle_log.size(), 2u);
    EXPECT_EQ(g_lifecycle_log[1], "pre_destroy");
}

// ─── Test bean: callback-based lifecycle ─────────────────────────────────────

struct SimpleBean {
    std::vector<std::string>* events = nullptr;
};

TEST(DILifecycle, CallbackBasedLifecycle) {
    std::vector<std::string> events;

    RootContainer root;
    root.register_bean<SimpleBean>([&events](ContainerBase&) {
        auto* b = new SimpleBean{};
        b->events = &events;
        return b;
    });
    root.with_post_construct<SimpleBean>([](SimpleBean& b) {
        b.events->push_back("post_construct");
    });
    root.with_pre_destroy<SimpleBean>([](SimpleBean& b) {
        b.events->push_back("pre_destroy");
    });

    root.build();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], "post_construct");

    root.shutdown();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1], "pre_destroy");
}

// ─── Reverse-order pre_destroy ─────────────────────────────────────────────

struct OrderedBean {
    std::vector<std::string>* log;
    std::string name;
    OrderedBean(std::vector<std::string>* l, std::string n) : log(l), name(std::move(n)) {}
};

TEST(DILifecycle, PreDestroyReverseOrder) {
    std::vector<std::string> destroyed;

    RootContainer root;

    // A depends on nothing
    root.register_bean<int>([](ContainerBase&) { return new int{0}; }); // placeholder

    // Register three beans A, B, C in order
    // We use different types via wrappers
    struct BeanA { std::vector<std::string>* log; };
    struct BeanB { std::vector<std::string>* log; };
    struct BeanC { std::vector<std::string>* log; };

    root.register_bean<BeanA>([&destroyed](ContainerBase&) {
        return new BeanA{&destroyed};
    });
    root.with_post_construct<BeanA>([](BeanA&) {});
    root.with_pre_destroy<BeanA>([](BeanA& b) { b.log->push_back("A"); });

    root.register_bean<BeanB>([&destroyed](ContainerBase&) {
        return new BeanB{&destroyed};
    });
    root.with_pre_destroy<BeanB>([](BeanB& b) { b.log->push_back("B"); });

    root.register_bean<BeanC>([&destroyed](ContainerBase&) {
        return new BeanC{&destroyed};
    });
    root.with_pre_destroy<BeanC>([](BeanC& b) { b.log->push_back("C"); });

    root.build();
    root.shutdown();

    // pre_destroy must be called in REVERSE registration order: C, B, A
    ASSERT_GE(destroyed.size(), 3u);
    // Find A, B, C positions
    auto pos_a = std::find(destroyed.begin(), destroyed.end(), "A") - destroyed.begin();
    auto pos_b = std::find(destroyed.begin(), destroyed.end(), "B") - destroyed.begin();
    auto pos_c = std::find(destroyed.begin(), destroyed.end(), "C") - destroyed.begin();
    EXPECT_GT(pos_a, pos_b) << "A must be destroyed after B";
    EXPECT_GT(pos_b, pos_c) << "B must be destroyed after C";
}

// ─── LifecycleManager unit tests ──────────────────────────────────────────────

TEST(LifecycleManager, InvokePostConstructs) {
    LifecycleManager lm;
    int counter = 0;
    int x = 0;
    lm.register_bean(&x, [&counter](void*){ ++counter; }, {});
    lm.register_bean(&x, [&counter](void*){ ++counter; }, {});
    lm.invoke_post_constructs();
    EXPECT_EQ(counter, 2);
}

TEST(LifecycleManager, InvokePreDestroysReverseOrder) {
    LifecycleManager lm;
    std::vector<int> order;
    int x = 0;
    lm.register_bean(&x, {}, [&order](void*){ order.push_back(1); });
    lm.register_bean(&x, {}, [&order](void*){ order.push_back(2); });
    lm.register_bean(&x, {}, [&order](void*){ order.push_back(3); });
    lm.invoke_pre_destroys();
    EXPECT_EQ(order, (std::vector<int>{3, 2, 1}));
}
