#include <gtest/gtest.h>
#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/annotations/scanner.h"

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;

// ─── Test Controllers & Advice ────────────────────────────────────────────────

struct [[= RestController("/api/autotest") ]] AutoTestController {
    [[= GetMapping("/hello") ]]
    std::string hello() {
        return "hello autotest";
    }
};

struct MyAutoException : public std::exception {
    const char* what() const noexcept override { return "auto error"; }
};

struct [[= ControllerAdvice() ]] AutoAdvice {
    [[= ExceptionHandler() ]]
    std::string handle_ex(const MyAutoException&) {
        return "handled auto error";
    }
};

#include <filesystem>

static std::string get_cert_path() {
    std::filesystem::path p = "cert.pem";
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(p)) return p.string();
        p = "../" / p;
    }
    return "";
}

static std::string get_key_path() {
    std::filesystem::path p = "key.pem";
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(p)) return p.string();
        p = "../" / p;
    }
    return "";
}

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST(AutoRoutingTest, ControllerAndAdviceAutoRegistration) {
    RootContainer di_root;

    // Scan and register
    register_beans<AutoTestController, AutoAdvice>(di_root);
    di_root.build();

    auto cert = get_cert_path();
    auto key = get_key_path();
    ASSERT_FALSE(cert.empty());
    ASSERT_FALSE(key.empty());

    // Setup Server using Server::Builder with the DI container
    auto server = Server::create()
        .tls(cert, key) // needed by build() validation
        .di_container(di_root)
        .build();

    // Verify GET route was auto-registered in the router
    http3::Request req;
    req.set_method("GET");
    req.set_path("/api/autotest/hello");
    http3::Response res;
    context::RequestContext ctx;

    // Simulate request handling via router
    auto match_result = server->router().match("GET", "/api/autotest/hello");
    ASSERT_NE(match_result.handler, nullptr);
    
    // Inject Controller and invoke handler
    auto shard_container = di_root.make_shard_container();
    shard_container->initialize();
    auto req_container = shard_container->make_request_container();
    ctx.bind_container(*req_container);

    (*match_result.handler)(req, res, ctx);
    EXPECT_EQ(res.status_code(), 200);
    EXPECT_EQ(res.body_str(), "\"hello autotest\"");

    // Verify ExceptionHandler was auto-registered in the router
    MyAutoException ex;
    bool handled = server->handle_exception(ex, res, ctx);
    EXPECT_TRUE(handled);
    EXPECT_EQ(res.status_code(), 200);
    EXPECT_EQ(res.body_str(), "\"handled auto error\"");

    di_root.shutdown();
}
