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

struct [[= RestController("/api/secure") ]]
       [[= Authorize("admin", "articles:write") ]]
DeclarativeSecurityController {
    [[= GetMapping("/inherited") ]]
    std::string inherited() { return "protected"; }

    [[= GetMapping("/auditor") ]]
    [[= Authorize("auditor") ]]
    std::string auditor() { return "auditor"; }

    [[= GetMapping("/public") ]]
    [[= PermitAll() ]]
    std::string public_endpoint() { return "public"; }
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

TEST(AutoRoutingTest, DeclarativeAuthorizationProtectsReflectedControllerRoutes) {
    RootContainer di_root;
    register_beans<DeclarativeSecurityController>(di_root);
    di_root.build();

    auto cert = get_cert_path();
    auto key = get_key_path();
    ASSERT_FALSE(cert.empty());
    ASSERT_FALSE(key.empty());
    auto server = Server::create().tls(cert, key).di_container(di_root).build();

    auto shard_container = di_root.make_shard_container();
    shard_container->initialize();
    auto request_container = shard_container->make_request_container();

    auto invoke = [&](std::string_view path, context::RequestContext& context) {
        http3::Request request;
        request.set_method("GET");
        request.set_path(std::string(path));
        http3::Response response;
        context.bind_container(*request_container);
        auto match = server->router().match("GET", path);
        EXPECT_NE(match.handler, nullptr);
        (*match.handler)(request, response, context);
        return response;
    };

    context::RequestContext anonymous;
    auto anonymous_response = invoke("/api/secure/inherited", anonymous);
    EXPECT_EQ(anonymous_response.status_code(), 401);

    context::RequestContext insufficient;
    middleware::JwtPrincipal reader;
    reader.subject = "reader";
    reader.claims.string_array_claims["roles"] = {"reader"};
    reader.scopes = {"articles:read"};
    insufficient.set(reader);
    auto forbidden_response = invoke("/api/secure/inherited", insufficient);
    EXPECT_EQ(forbidden_response.status_code(), 403);

    context::RequestContext admin;
    middleware::JwtPrincipal writer;
    writer.subject = "writer";
    writer.claims.string_array_claims["roles"] = {"admin"};
    writer.scopes = {"articles:write"};
    admin.set(writer);
    auto allowed_response = invoke("/api/secure/inherited", admin);
    EXPECT_EQ(allowed_response.status_code(), 200);
    EXPECT_EQ(allowed_response.body_str(), "\"protected\"");

    context::RequestContext auditor;
    middleware::SessionPrincipal auditor_principal{
        .subject = "auditor", .roles = {"auditor"}, .scopes = {}};
    auditor.set(auditor_principal);
    auto auditor_response = invoke("/api/secure/auditor", auditor);
    EXPECT_EQ(auditor_response.status_code(), 200);
    EXPECT_EQ(auditor_response.body_str(), "\"auditor\"");

    context::RequestContext public_context;
    auto public_response = invoke("/api/secure/public", public_context);
    EXPECT_EQ(public_response.status_code(), 200);
    EXPECT_EQ(public_response.body_str(), "\"public\"");

    di_root.shutdown();
}
