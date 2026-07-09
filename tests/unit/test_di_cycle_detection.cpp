/// @file test_di_cycle_detection.cpp
/// Tests that circular dependencies are detected at build() time.
///
/// To inject a cycle manually, we use dep_type_ids on BeanRegistration via
/// a helper wrapper that registers beans WITH explicit dep_type_ids.
/// Since registrations_ is protected, we test via public API only.
///
/// The cycle: if A's factory tries to resolve B and B's factory tries to
/// resolve A, this causes an infinite loop/stack overflow at build() in
/// a naive DI container. Our container detects cycles at build() time
/// using DFS on dep_type_ids which are populated by the codegen or scanner.
///
/// For runtime-only registration (no dep_type_ids), we test the graph
/// by using a subclass that adds dep_type_ids.

#include <gtest/gtest.h>
#include "novaboot/di/container.h"

using namespace novaboot::di;

// ─── Helper: container that exposes registration manipulation ─────────────────

class TestableContainer : public RootContainer {
public:
    void add_dependency(std::type_index from, std::type_index to) {
        auto it = registrations_.find(from);
        if (it != registrations_.end())
            it->second.dep_type_ids.push_back(to);
    }
};

// ─── Beans ────────────────────────────────────────────────────────────────────

struct CycleA { int v = 1; };
struct CycleB { int v = 2; };
struct CycleC { int v = 3; };

// ─── Direct cycle: A → B → A ─────────────────────────────────────────────────

TEST(DICycleDetection, DirectCycleThrowsOnBuild) {
    TestableContainer root;

    root.register_bean<CycleA>([](ContainerBase&) { return new CycleA{}; });
    root.register_bean<CycleB>([](ContainerBase&) { return new CycleB{}; });

    // Wire cycle: A depends on B, B depends on A
    root.add_dependency(std::type_index(typeid(CycleA)), std::type_index(typeid(CycleB)));
    root.add_dependency(std::type_index(typeid(CycleB)), std::type_index(typeid(CycleA)));

    EXPECT_THROW(root.build(), DIError)
        << "Direct cycle A→B→A must throw DIError at build()";
}

// ─── Transitive cycle: X → Y → Z → X ────────────────────────────────────────

TEST(DICycleDetection, TransitiveCycleThrowsOnBuild) {
    struct X { int v = 0; };
    struct Y { int v = 0; };
    struct Z { int v = 0; };

    TestableContainer root;
    root.register_bean<X>([](ContainerBase&) { return new X{}; });
    root.register_bean<Y>([](ContainerBase&) { return new Y{}; });
    root.register_bean<Z>([](ContainerBase&) { return new Z{}; });

    root.add_dependency(std::type_index(typeid(X)), std::type_index(typeid(Y)));
    root.add_dependency(std::type_index(typeid(Y)), std::type_index(typeid(Z)));
    root.add_dependency(std::type_index(typeid(Z)), std::type_index(typeid(X)));  // cycle

    EXPECT_THROW(root.build(), DIError)
        << "Transitive cycle X→Y→Z→X must throw DIError";
}

// ─── No cycle: Engine → nothing, Car → Engine ────────────────────────────────

struct Engine2 { int power = 100; };
struct Car2 {
    Engine2& eng;
    explicit Car2(Engine2& e) : eng(e) {}
};

TEST(DICycleDetection, NoCycleBuildsSuccessfully) {
    RootContainer root;
    root.register_bean<Engine2>([](ContainerBase&) { return new Engine2{}; });
    root.register_bean<Car2>([](ContainerBase& c) {
        return new Car2{c.resolve<Engine2>()};
    });

    EXPECT_NO_THROW(root.build());
    auto& car = root.resolve<Car2>();
    EXPECT_EQ(car.eng.power, 100);
}

// ─── Diamond dependency (no cycle) ───────────────────────────────────────────
// Logger ←── Service1
// Logger ←── Service2
// Service1, Service2 ←── App

struct Logger2  { int id = 1; };
struct Service1 { Logger2& log; explicit Service1(Logger2& l) : log(l) {} };
struct Service2 { Logger2& log; explicit Service2(Logger2& l) : log(l) {} };
struct App2     { Service1& s1; Service2& s2; App2(Service1& a, Service2& b): s1(a), s2(b) {} };

TEST(DICycleDetection, DiamondDependencyNoCycle) {
    RootContainer root;
    root.register_bean<Logger2>([](ContainerBase&) { return new Logger2{}; });
    root.register_bean<Service1>([](ContainerBase& c) {
        return new Service1{c.resolve<Logger2>()};
    });
    root.register_bean<Service2>([](ContainerBase& c) {
        return new Service2{c.resolve<Logger2>()};
    });
    root.register_bean<App2>([](ContainerBase& c) {
        return new App2{c.resolve<Service1>(), c.resolve<Service2>()};
    });

    EXPECT_NO_THROW(root.build());

    auto& app = root.resolve<App2>();
    // Both services share the same Logger2 singleton
    EXPECT_EQ(&app.s1.log, &app.s2.log) << "Diamond deps share the Logger2 singleton";
}
