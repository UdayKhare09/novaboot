#include <gtest/gtest.h>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/cookie.h"
#include "novaboot/middleware/session_middleware.h"

namespace {

novaboot::middleware::SessionPrincipal principal() {
    return {
        .subject = "user-42",
        .roles = {"user"},
        .scopes = {"articles:write"},
    };
}

std::string session_cookie_value(const novaboot::http3::Response& response) {
    const auto field = response.headers().get("set-cookie");
    EXPECT_TRUE(field.has_value());
    const auto equals = field->find('=');
    const auto end = field->find(';');
    return std::string(field->substr(equals + 1, end - equals - 1));
}

TEST(SessionManagerTest, LoginRotatesExistingSessionAndUsesSecureCookieDefaults) {
    novaboot::middleware::SessionManager sessions;
    novaboot::http3::Request first_request;
    novaboot::http3::Response first_response;
    const auto first = sessions.login(first_request, first_response, principal());

    EXPECT_EQ(first.id.size(), 64U);
    EXPECT_NE(first_response.headers().get("set-cookie")->find("Secure"), std::string_view::npos);
    EXPECT_NE(first_response.headers().get("set-cookie")->find("HttpOnly"), std::string_view::npos);
    EXPECT_NE(first_response.headers().get("set-cookie")->find("SameSite=Lax"), std::string_view::npos);

    novaboot::http3::Request login_again;
    login_again.headers().set("cookie", "NOVA_SESSION=" + session_cookie_value(first_response));
    novaboot::http3::Response second_response;
    const auto second = sessions.login(login_again, second_response, principal());

    EXPECT_NE(first.id, second.id);
    EXPECT_FALSE(sessions.authenticate(login_again));
}

TEST(SessionManagerTest, LogoutInvalidatesServerSessionAndExpiresCookie) {
    novaboot::middleware::SessionManager sessions;
    novaboot::http3::Request initial;
    novaboot::http3::Response login_response;
    sessions.login(initial, login_response, principal());

    novaboot::http3::Request request;
    request.headers().set("cookie", "NOVA_SESSION=" + session_cookie_value(login_response));
    ASSERT_TRUE(sessions.authenticate(request));
    novaboot::http3::Response logout_response;
    sessions.logout(request, logout_response);

    EXPECT_FALSE(sessions.authenticate(request));
    ASSERT_TRUE(logout_response.headers().get("set-cookie"));
    EXPECT_NE(logout_response.headers().get("set-cookie")->find("Max-Age=0"), std::string_view::npos);
}

TEST(SessionMiddlewareTest, PublishesPrincipalForAValidSession) {
    auto sessions = std::make_shared<novaboot::middleware::SessionManager>();
    novaboot::http3::Request initial;
    novaboot::http3::Response login_response;
    sessions->login(initial, login_response, principal());

    novaboot::http3::Request request;
    request.set_method("GET");
    request.set_path("/articles");
    request.headers().set("cookie", "NOVA_SESSION=" + session_cookie_value(login_response));
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;
    novaboot::middleware::SessionMiddleware middleware(sessions);

    middleware.handle(request, response, context, [&] { called = true; });

    EXPECT_TRUE(called);
    ASSERT_NE(context.get<novaboot::middleware::SessionPrincipal>(), nullptr);
    EXPECT_EQ(context.get<novaboot::middleware::SessionPrincipal>()->subject, "user-42");
}

TEST(SessionMiddlewareTest, RejectsMissingSessionExceptForAllowlistedPath) {
    auto sessions = std::make_shared<novaboot::middleware::SessionManager>();
    novaboot::middleware::SessionMiddleware middleware(
        sessions, {.allowlist_paths = {"/login"}});
    novaboot::http3::Request request;
    request.set_method("POST");
    request.set_path("/private");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;
    middleware.handle(request, response, context, [&] { called = true; });
    EXPECT_FALSE(called);
    EXPECT_EQ(response.status_code(), 401);

    request.set_path("/login");
    response = {};
    middleware.handle(request, response, context, [&] { called = true; });
    EXPECT_TRUE(called);
}

} // namespace
