#include <gtest/gtest.h>
#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/annotations/scanner.h"

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;

// ─── Test Exceptions ──────────────────────────────────────────────────────────

class AdviceTestException1 : public std::runtime_error {
public:
    explicit AdviceTestException1(const std::string& msg) : std::runtime_error(msg) {}
};

class AdviceTestException2 : public std::runtime_error {
public:
    explicit AdviceTestException2(const std::string& msg) : std::runtime_error(msg) {}
};

// ─── Test Advice ──────────────────────────────────────────────────────────────

static int g_advice_calls_1 = 0;
static int g_advice_calls_2 = 0;

struct [[= ControllerAdvice() ]] MyTestAdvice {
    [[= ExceptionHandler() ]]
    ResponseEntity<std::string> handle_ex1(const AdviceTestException1& ex, context::RequestContext&) {
        g_advice_calls_1++;
        return ResponseEntity<std::string>::status(418, "Handled1: " + std::string(ex.what()));
    }

    [[= ExceptionHandler() ]]
    void handle_ex2(const AdviceTestException2&) {
        g_advice_calls_2++;
    }
};

// ─── Verification Static Asserts ──────────────────────────────────────────────

static_assert(has_annotation<ControllerAdvice>(^^MyTestAdvice), "MyTestAdvice must have ControllerAdvice annotation");

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST(ControllerAdviceTest, ReflectionAndRouting) {
    g_advice_calls_1 = 0;
    g_advice_calls_2 = 0;

    RootContainer di_root;
    register_beans<MyTestAdvice>(di_root);
    di_root.build();

    // Verify DI registered the advice
    EXPECT_TRUE(di_root.has<MyTestAdvice>());

    router::Router router;
    register_advice<MyTestAdvice>(router);

    // Prepare context and request/response
    auto shard = di_root.make_shard_container();
    auto req_container = shard->make_request_container();
    context::RequestContext ctx;
    ctx.bind_container(*req_container);

    http3::Request req;
    http3::Response res;

    // Test Exception 1
    AdviceTestException1 ex1("coffee");
    bool handled1 = router.handle_exception(ex1, res, ctx);
    EXPECT_TRUE(handled1);
    EXPECT_EQ(g_advice_calls_1, 1);
    EXPECT_EQ(res.status_code(), 418);
    EXPECT_EQ(res.body_data(), "\"Handled1: coffee\""); // serialized json string

    // Test Exception 2 (void return handler)
    AdviceTestException2 ex2("tea");
    bool handled2 = router.handle_exception(ex2, res, ctx);
    EXPECT_TRUE(handled2);
    EXPECT_EQ(g_advice_calls_2, 1);
}
