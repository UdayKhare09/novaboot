#include <gtest/gtest.h>

#include "novaboot/http/content_negotiation.h"
#include "novaboot/middleware/content_negotiation_middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/router/route.h"

namespace {

using novaboot::context::RequestContext;
using novaboot::http3::Request;
using novaboot::http3::Response;

std::optional<std::string_view> select(Request& request) {
    return novaboot::http::negotiate_content_type(
        request, {"application/json", "text/plain"});
}

TEST(ContentNegotiation, UsesHighestAcceptQuality) {
    Request request;
    request.headers().set("Accept", "text/plain;q=0.4, application/json;q=0.9");
    ASSERT_EQ(select(request), std::optional<std::string_view>{"application/json"});
}

TEST(ContentNegotiation, SupportsWildcardsAndAbsentAccept) {
    Request wildcard;
    wildcard.headers().set("accept", "application/*");
    ASSERT_EQ(select(wildcard), std::optional<std::string_view>{"application/json"});

    Request absent;
    ASSERT_EQ(select(absent), std::optional<std::string_view>{"application/json"});
}

TEST(ContentNegotiation, RejectsZeroQualityAndInvalidRanges) {
    Request request;
    request.headers().set("accept", "application/json;q=0, image/png");
    EXPECT_FALSE(select(request).has_value());
}

TEST(ContentNegotiation, MiddlewareReturns406ForIncompatibleResponse) {
    novaboot::middleware::Pipeline pipeline;
    pipeline.add(std::make_shared<novaboot::middleware::ContentNegotiationMiddleware>());
    Request request;
    Response response;
    RequestContext context;
    request.headers().set("accept", "application/json");

    novaboot::router::Handler handler =
        [](Request&, Response& output, RequestContext&) {
            output.text("plain response");
        };
    pipeline.execute(request, response, context, handler);

    EXPECT_EQ(response.status_code(), 406);
    EXPECT_EQ(response.body_str(), "Not Acceptable");
    EXPECT_EQ(response.headers().get("content-type"),
              std::optional<std::string_view>{"text/plain; charset=utf-8"});
    EXPECT_EQ(response.headers().get("vary"), std::optional<std::string_view>{"Accept"});
}

TEST(ContentNegotiation, TextAndDownloadSetSafeRepresentationHeaders) {
    Response response;
    response.text("hello").download("report\r\nInjected: yes.pdf", "application/pdf");

    EXPECT_EQ(response.headers().get("content-type"),
              std::optional<std::string_view>{"application/pdf"});
    EXPECT_EQ(response.headers().get("content-disposition"),
              std::optional<std::string_view>{"attachment; filename=\"report__Injected_ yes.pdf\""});
}

} // namespace
