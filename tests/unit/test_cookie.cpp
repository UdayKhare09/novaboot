#include <gtest/gtest.h>

#include "novaboot/http3/cookie.h"

namespace {

TEST(CookieTest, SerializesSafeCookieAttributes) {
    novaboot::http3::Cookie cookie{
        .name = "nova_session",
        .value = "abc.def_123",
        .path = "/app",
        .domain = "example.test",
        .max_age = std::chrono::seconds{3600},
        .secure = true,
        .http_only = true,
        .same_site = novaboot::http3::SameSite::Strict,
    };

    EXPECT_EQ(novaboot::http3::serialize_cookie(cookie),
              "nova_session=abc.def_123; Path=/app; Domain=example.test; Max-Age=3600; Secure; HttpOnly; SameSite=Strict");
}

TEST(CookieTest, AddsAndReadsMultipleCookies) {
    novaboot::http3::Response response;
    novaboot::http3::set_cookie(response, {
        .name = "one", .value = "1", .domain = std::nullopt, .max_age = std::nullopt});
    novaboot::http3::set_cookie(response, {
        .name = "two", .value = "2", .domain = std::nullopt, .max_age = std::nullopt});
    EXPECT_EQ(response.headers().get_all("set-cookie").size(), 2U);

    novaboot::http3::Request request;
    request.headers().add("Cookie", "theme=dark; session=opaque-token");
    request.headers().add("Cookie", "locale=en-US");
    ASSERT_TRUE(novaboot::http3::request_cookie(request, "session"));
    EXPECT_EQ(*novaboot::http3::request_cookie(request, "session"), "opaque-token");
    EXPECT_EQ(*novaboot::http3::request_cookie(request, "locale"), "en-US");
}

TEST(CookieTest, RejectsUnsafeAttributes) {
    EXPECT_THROW(static_cast<void>(novaboot::http3::serialize_cookie({
                     .name = "bad;name", .value = "x", .domain = std::nullopt,
                     .max_age = std::nullopt})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(novaboot::http3::serialize_cookie({
                     .name = "safe", .value = "bad\r\nvalue", .domain = std::nullopt,
                     .max_age = std::nullopt})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(novaboot::http3::serialize_cookie({
                     .name = "safe", .value = "x", .domain = std::nullopt,
                     .max_age = std::nullopt, .secure = false,
                     .same_site = novaboot::http3::SameSite::None})),
                 std::invalid_argument);
}

} // namespace
