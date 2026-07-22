#include <gtest/gtest.h>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/cookie.h"
#include "novaboot/middleware/csrf_middleware.h"

namespace {

TEST(CsrfMiddlewareTest, SafeRequestIssuesReadableTokenCookie) {
    novaboot::middleware::CsrfMiddleware middleware;
    novaboot::http3::Request request;
    request.set_method("GET");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;

    middleware.handle(request, response, context, [&] { called = true; });

    EXPECT_TRUE(called);
    const auto cookies = response.headers().get_all("set-cookie");
    ASSERT_EQ(cookies.size(), 1U);
    EXPECT_TRUE(cookies.front().starts_with("XSRF-TOKEN="));
    EXPECT_NE(cookies.front().find("Secure"), std::string_view::npos);
    EXPECT_EQ(cookies.front().find("HttpOnly"), std::string_view::npos);
}

TEST(CsrfMiddlewareTest, UnsafeRequestWithoutMatchingTokenIsRejected) {
    novaboot::middleware::CsrfMiddleware middleware;
    novaboot::http3::Request request;
    request.set_method("POST");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;

    middleware.handle(request, response, context, [&] { called = true; });

    EXPECT_FALSE(called);
    EXPECT_EQ(response.status_code(), 403);
    EXPECT_EQ(response.body_data(), "{\"error\":\"CSRF token invalid or missing\"}");
}

TEST(CsrfMiddlewareTest, UnsafeRequestWithMatchingCookieAndHeaderProceeds) {
    novaboot::middleware::CsrfMiddleware middleware;
    novaboot::http3::Request request;
    request.set_method("PATCH");
    request.headers().set("cookie", "XSRF-TOKEN=matching-token");
    request.headers().set("x-xsrf-token", "matching-token");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;

    middleware.handle(request, response, context, [&] { called = true; });

    EXPECT_TRUE(called);
    EXPECT_EQ(response.status_code(), 200);
}

TEST(CsrfMiddlewareTest, ConfigurationRejectsInsecureSameSiteNoneCookie) {
    EXPECT_THROW(novaboot::middleware::CsrfMiddleware({
                     .secure_cookie = false,
                     .same_site = novaboot::http3::SameSite::None}),
                 std::invalid_argument);
}

} // namespace
