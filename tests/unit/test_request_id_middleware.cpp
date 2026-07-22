#include <gtest/gtest.h>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/middleware/request_id_middleware.h"

namespace {

TEST(RequestIdMiddlewareTest, GeneratesAndPublishesANewRequestId) {
    novaboot::middleware::Pipeline pipeline;
    pipeline.add(std::make_shared<novaboot::middleware::RequestIdMiddleware>());
    novaboot::http3::Request request;
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    std::string id_seen_by_handler;
    novaboot::router::Handler handler = [&](auto&, auto&, auto& handler_context) {
        id_seen_by_handler = handler_context.get_string("request_id");
    };

    pipeline.execute(request, response, context, handler);

    ASSERT_TRUE(response.headers().get("x-request-id"));
    EXPECT_EQ(id_seen_by_handler, *response.headers().get("x-request-id"));
    EXPECT_EQ(id_seen_by_handler.size(), 32U);
}

TEST(RequestIdMiddlewareTest, ReusesOnlySafeInboundRequestId) {
    novaboot::middleware::RequestIdMiddleware middleware;
    novaboot::http3::Request request;
    request.headers().set("x-request-id", "edge-gateway-42");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    middleware.handle(request, response, context, [] {});
    EXPECT_EQ(context.get_string("request_id"), "edge-gateway-42");

    request.headers().set("x-request-id", "bad\r\nvalue");
    response = {};
    context.clear();
    middleware.handle(request, response, context, [] {});
    EXPECT_NE(context.get_string("request_id"), "bad\r\nvalue");
    EXPECT_EQ(context.get_string("request_id").size(), 32U);
}

} // namespace
