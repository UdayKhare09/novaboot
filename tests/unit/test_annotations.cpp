#include <gtest/gtest.h>
#include "novaboot/novaboot.h"

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;

// ─── Test Components ──────────────────────────────────────────────────────────

static int g_repository_calls = 0;
static int g_post_construct_calls = 0;
static int g_pre_destroy_calls = 0;
static int g_controller_calls = 0;

struct [[= Repository() ]] TestRepository {
    void do_db_work() {
        g_repository_calls++;
    }
};

struct [[= Service() ]] TestService {
    TestRepository& repo;
    
    explicit TestService(TestRepository& r) : repo(r) {}
    
    void do_service_work() {
        repo.do_db_work();
    }
    
    [[= PostConstruct() ]]
    void init() {
        g_post_construct_calls++;
    }
    
    [[= PreDestroy() ]]
    void destroy() {
        g_pre_destroy_calls++;
    }
};

struct [[= RestController("/api/test") ]] TestController {
    TestService& service;
    
    explicit TestController(TestService& s) : service(s) {}
    
    [[= GetMapping("/hello")]]
    void hello(http3::Request&, http3::Response&, context::RequestContext&) {
        g_controller_calls++;
        service.do_service_work();
    }
    
    [[= PostMapping("/create")]]
    void create(http3::Request&, http3::Response&, context::RequestContext&) {
        g_controller_calls += 10;
        service.do_service_work();
    }
};

// ─── Tests ────────────────────────────────────────────────────────────────────

static_assert(has_annotation<Repository>(^^TestRepository), "TestRepository should have Repository annotation");
static_assert(has_annotation<Service>(^^TestService), "TestService should have Service annotation");
static_assert(has_annotation<RestController>(^^TestController), "TestController should have RestController annotation");

TEST(AnnotationsTest, FullLifecycleAndRouteScanning) {
    g_repository_calls = 0;
    g_post_construct_calls = 0;
    g_pre_destroy_calls = 0;
    g_controller_calls = 0;

    RootContainer di_root;
    
    // Register beans using reflection scanner
    register_beans<TestRepository, TestService, TestController>(di_root);
    
    di_root.build();
    
    // Verify PostConstruct was invoked
    EXPECT_EQ(g_post_construct_calls, 1);
    
    // Verify beans exist and are wired
    EXPECT_TRUE(di_root.has<TestRepository>());
    EXPECT_TRUE(di_root.has<TestService>());
    EXPECT_TRUE(di_root.has<TestController>());
    
    auto& controller = di_root.resolve<TestController>();
    auto& service = di_root.resolve<TestService>();
    EXPECT_EQ(&controller.service, &service);
    
    // Verify routes can be registered and resolved
    router::Router router;
    register_routes<TestController>(router);
    
    // Verify the routes exist in the router
    EXPECT_EQ(router.size(), 2u);
    
    // Match GET /api/test/hello
    auto match_get = router.match(router::Method::GET, "/api/test/hello");
    ASSERT_NE(match_get.handler, nullptr);
    
    // Match POST /api/test/create
    auto match_post = router.match(router::Method::POST, "/api/test/create");
    ASSERT_NE(match_post.handler, nullptr);
    
    // Invoke handlers
    auto shard = di_root.make_shard_container();
    auto req_container = shard->make_request_container();
    context::RequestContext ctx;
    ctx.bind_container(*req_container);
    
    http3::Request req;
    http3::Response res;
    
    (*match_get.handler)(req, res, ctx);
    EXPECT_EQ(g_controller_calls, 1);
    EXPECT_EQ(g_repository_calls, 1);
    
    (*match_post.handler)(req, res, ctx);
    EXPECT_EQ(g_controller_calls, 11);
    EXPECT_EQ(g_repository_calls, 2);
    
    // Shutdown container and verify PreDestroy
    di_root.shutdown();
    EXPECT_EQ(g_pre_destroy_calls, 1);
}
