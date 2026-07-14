#include <gtest/gtest.h>
#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/annotations/scanner.h"
#include <memory>

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;

// ─── Test Classes ────────────────────────────────────────────────────────────

struct Dependency1 {
    int val = 101;
};

struct [[= Component() ]] Dependency2 {
    int val = 202;
};

struct TargetBean {
    std::shared_ptr<Dependency1> dep1;
    Dependency2& dep2;
    TargetBean(std::shared_ptr<Dependency1> d1, Dependency2& d2) : dep1(std::move(d1)), dep2(d2) {}
    
    bool is_initialized = false;
    
    [[= PostConstruct() ]]
    void init() {
        is_initialized = true;
    }
};

struct [[= Configuration() ]] MyConfig {
    [[= Bean() ]]
    TargetBean* create_target_bean(std::shared_ptr<Dependency1> d1, Dependency2& d2) {
        return new TargetBean(std::move(d1), d2);
    }

    [[= Bean() ]]
    std::shared_ptr<Dependency1> custom_dependency() {
        return std::make_shared<Dependency1>(Dependency1{ .val = 999 });
    }
};

// ─── Verification Static Asserts ──────────────────────────────────────────────

static_assert(has_annotation<Configuration>(^^MyConfig), "MyConfig must have Configuration annotation");

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST(DiConfigBeanTest, ConfigurationAndBeanAutowiring) {
    RootContainer di_root;

    // Scan and register Dependency2 and MyConfig
    register_beans<Dependency2, MyConfig>(di_root);

    di_root.build();

    // Verify registrations exist
    EXPECT_TRUE(di_root.has<MyConfig>());
    EXPECT_TRUE(di_root.has<std::shared_ptr<Dependency1>>());
    EXPECT_TRUE(di_root.has<Dependency2>());
    EXPECT_TRUE(di_root.has<TargetBean>());

    // Verify injected dependencies and method invocations
    auto& target = di_root.resolve<TargetBean>();
    EXPECT_EQ(target.dep1->val, 999); // custom_dependency returned std::shared_ptr
    EXPECT_EQ(target.dep2.val, 202); // Dependency2 autowired as Singleton Component

    // Verify lifecycle hook run on `@Bean` registered class
    EXPECT_TRUE(target.is_initialized);

    di_root.shutdown();
}
