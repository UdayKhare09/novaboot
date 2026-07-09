/// @file test_di_async.cpp
/// Tests for async bean initialization (factory returns std::future<T*>).

#include <gtest/gtest.h>
#include "novaboot/di/container.h"
#include "novaboot/di/inject.h"

#include <chrono>
#include <thread>

using namespace novaboot::di;
using namespace std::chrono_literals;

// ─── Async bean ───────────────────────────────────────────────────────────────

struct HeavyService {
    int data;
    std::chrono::milliseconds init_time;
    explicit HeavyService(int d, std::chrono::milliseconds t) : data(d), init_time(t) {}
};

TEST(DIAsync, AsyncBeanAvailableAfterBuild) {
    RootContainer root;

    root.register_async_bean<HeavyService>(
        [](ContainerBase&) -> std::future<HeavyService*> {
            return std::async(std::launch::async, []() -> HeavyService* {
                std::this_thread::sleep_for(50ms);  // Simulate slow init
                return new HeavyService{99, 50ms};
            });
        },
        /*timeout_ms=*/ 5000u
    );

    EXPECT_NO_THROW(root.build());  // build() awaits the async bean

    auto& svc = root.resolve<HeavyService>();
    EXPECT_EQ(svc.data, 99);
}

TEST(DIAsync, AsyncBeanTimeoutThrows) {
    RootContainer root;

    root.register_async_bean<HeavyService>(
        [](ContainerBase&) -> std::future<HeavyService*> {
            return std::async(std::launch::async, []() -> HeavyService* {
                std::this_thread::sleep_for(2000ms);  // Way too slow
                return new HeavyService{0, 2000ms};
            });
        },
        /*timeout_ms=*/ 10u  // 10ms timeout — will expire
    );

    EXPECT_THROW(root.build(), DIError)
        << "Async bean exceeding timeout must throw DIError";
}

TEST(DIAsync, AsyncBeanIsSingleton) {
    RootContainer root;

    root.register_async_bean<HeavyService>(
        [](ContainerBase&) -> std::future<HeavyService*> {
            return std::async(std::launch::async, []() -> HeavyService* {
                return new HeavyService{7, 0ms};
            });
        }
    );

    root.build();
    auto& s1 = root.resolve<HeavyService>();
    auto& s2 = root.resolve<HeavyService>();
    EXPECT_EQ(&s1, &s2) << "Async bean is still a Singleton";
    EXPECT_EQ(s1.data, 7);
}

// ─── Lazy bean ────────────────────────────────────────────────────────────────

struct LazyComputer {
    static int instances;
    int id;
    LazyComputer() : id(++instances) {}
    ~LazyComputer() { --instances; }
};
int LazyComputer::instances = 0;

TEST(DIAsync, LazyBeanNotCreatedAtBuild) {
    LazyComputer::instances = 0;
    RootContainer root;

    root.register_bean<LazyComputer>(
        [](ContainerBase&) { return new LazyComputer{}; },
        Scope::Singleton,
        /*qualifier=*/ "",
        /*is_primary=*/ false,
        /*is_lazy=*/ true
    );

    root.build();
    EXPECT_EQ(LazyComputer::instances, 0) << "Lazy bean must NOT be created at build()";

    auto& c = root.resolve<LazyComputer>();
    EXPECT_EQ(LazyComputer::instances, 1) << "Lazy bean created on first resolve()";
    EXPECT_EQ(c.id, 1);
}
