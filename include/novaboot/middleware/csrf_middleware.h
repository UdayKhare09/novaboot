#pragma once

#include <string>

#include "novaboot/http3/cookie.h"
#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Double-submit CSRF protection for browser routes authenticated by cookies.
///
/// Safe requests receive an `XSRF-TOKEN` cookie when they do not already have
/// one. Unsafe requests must send the same token in the configured header.
/// Install this middleware on cookie-authenticated browser routes; bearer-token
/// APIs normally do not need CSRF protection.
class CsrfMiddleware final : public Middleware {
public:
    struct Config {
        std::string cookie_name = "XSRF-TOKEN";
        std::string header_name = "X-XSRF-TOKEN";
        std::string cookie_path = "/";
        bool secure_cookie = true;
        http::SameSite same_site = http::SameSite::Lax;
        bool issue_token_on_safe_methods = true;
    };

    CsrfMiddleware();
    explicit CsrfMiddleware(Config config);

    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;

private:
    [[nodiscard]] static bool safe_method(std::string_view method) noexcept;
    [[nodiscard]] static std::string generate_token();
    [[nodiscard]] bool valid_token(const http3::Request& request) const noexcept;
    void issue_token(http3::Response& response) const;

    Config config_;
};

} // namespace novaboot::middleware
