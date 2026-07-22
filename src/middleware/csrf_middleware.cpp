#include "novaboot/middleware/csrf_middleware.h"

#include <stdexcept>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace novaboot::middleware {

CsrfMiddleware::CsrfMiddleware() : CsrfMiddleware(Config{}) {}

CsrfMiddleware::CsrfMiddleware(Config config) : config_(std::move(config)) {
    if (config_.cookie_name.empty() || config_.header_name.empty() ||
        config_.cookie_path.empty() || config_.cookie_path.front() != '/') {
        throw std::invalid_argument("CSRF cookie name, header name, and absolute path are required");
    }
    if (config_.same_site == http::SameSite::None && !config_.secure_cookie) {
        throw std::invalid_argument("CSRF SameSite=None cookie must be Secure");
    }
}

bool CsrfMiddleware::safe_method(std::string_view method) noexcept {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

std::string CsrfMiddleware::generate_token() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("OpenSSL could not generate a CSRF token");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(bytes) * 2);
    for (const auto value : bytes) {
        token.push_back(hex[value >> 4]);
        token.push_back(hex[value & 0x0f]);
    }
    return token;
}

bool CsrfMiddleware::valid_token(const http3::Request& request) const noexcept {
    const auto cookie = http::request_cookie(request, config_.cookie_name);
    const auto header = request.header(config_.header_name);
    if (!cookie || !header || cookie->empty() || cookie->size() != header->size()) return false;
    return CRYPTO_memcmp(cookie->data(), header->data(), cookie->size()) == 0;
}

void CsrfMiddleware::issue_token(http3::Response& response) const {
    http::set_cookie(response, http::Cookie{
        .name = config_.cookie_name,
        .value = generate_token(),
        .path = config_.cookie_path,
        .domain = std::nullopt,
        .max_age = std::nullopt,
        .secure = config_.secure_cookie,
        // Browser JavaScript must read the token to echo it in the request
        // header. The authentication/session cookie remains HttpOnly.
        .http_only = false,
        .same_site = config_.same_site,
    });
}

void CsrfMiddleware::handle(http3::Request& request, http3::Response& response,
                            context::RequestContext&, Next next) {
    if (safe_method(request.method())) {
        if (config_.issue_token_on_safe_methods &&
            !http::request_cookie(request, config_.cookie_name)) {
            issue_token(response);
        }
        next();
        return;
    }

    if (!valid_token(request)) {
        response.status(403).json("{\"error\":\"CSRF token invalid or missing\"}");
        return;
    }
    next();
}

} // namespace novaboot::middleware
