#include <gtest/gtest.h>
#include "novaboot/core/server.h"
#include "novaboot/di/di.h"
#include "novaboot/router/web_attributes.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/context/request_context.h"
#include <stdexcept>
#include <string>

class MyTestException : public std::runtime_error {
public:
    explicit MyTestException(const std::string& msg) : std::runtime_error(msg) {}
};

class OtherException : public std::runtime_error {
public:
    explicit OtherException(const std::string& msg) : std::runtime_error(msg) {}
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

struct [[=novaboot::di::component{}]]
       [[=novaboot::web::controller_advice{}]] MyTestAdvice {
    MyTestAdvice() = default;

    [[=novaboot::web::exception_handler{^^MyTestException}]]
    auto handle_test_exception(const MyTestException& ex, novaboot::context::RequestContext&) {
        return novaboot::ResponseEntity<std::string>::status(418, "Tea: " + std::string(ex.what()));
    }
};

TEST(ExceptionHandlerTest, MatchAndExecuteAdvice) {
    novaboot::di::RootContainer di_root;
    di_root.register_component<MyTestAdvice>();
    di_root.build();

    auto app = novaboot::Server::create()
        .di_container(di_root)
        .tls(get_cert_path(), get_key_path())
        .build();

    app->register_advices<MyTestAdvice>();

    // Prepare dummy request/response/context
    novaboot::http3::Request req;
    novaboot::http3::Response res;
    novaboot::context::RequestContext ctx;
    
    // Bind container to context to resolve advice instance
    auto shard = di_root.make_shard_container();
    shard->initialize();
    auto req_di = shard->make_request_container();
    ctx.bind_container(*req_di);

    // 1. Unhandled exception
    OtherException other("ignored");
    bool handled_other = app->handle_exception(other, res, ctx);
    EXPECT_FALSE(handled_other);

    // 2. Handled exception
    MyTestException my_ex("boiling");
    bool handled_my = app->handle_exception(my_ex, res, ctx);
    EXPECT_TRUE(handled_my);

    // Verify response properties mapped from ResponseEntity
    EXPECT_EQ(res.status_code(), 418);
    EXPECT_EQ(res.body_data(), "\"Tea: boiling\""); // JSON serialized string
}
