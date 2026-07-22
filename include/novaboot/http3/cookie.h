#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::http {

/// SameSite attribute used by a browser when deciding whether to send a cookie.
enum class SameSite {
    Strict,
    Lax,
    None,
};

/// A deliberately small, safe Set-Cookie representation.
///
/// Values are emitted verbatim, so control characters and cookie delimiters are
/// rejected rather than silently encoded. This keeps the helper suitable for
/// session identifiers, JWTs, and CSRF tokens without hiding wire behaviour.
struct Cookie {
    std::string name;
    std::string value;
    std::string path = "/";
    std::optional<std::string> domain;
    std::optional<std::chrono::seconds> max_age;
    bool secure = true;
    bool http_only = true;
    SameSite same_site = SameSite::Lax;
};

namespace detail {

inline bool cookie_token_character(unsigned char value) noexcept {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           value == '!' || value == '#' || value == '$' || value == '%' ||
           value == '&' || value == '\'' || value == '*' || value == '+' ||
           value == '-' || value == '.' || value == '^' || value == '_' ||
           value == '`' || value == '|' || value == '~';
}

inline bool contains_cookie_delimiter(std::string_view value) noexcept {
    for (const unsigned char character : value) {
        if (character <= 0x1f || character == 0x7f || character == ';' ||
            character == ',' || character == '\r' || character == '\n') {
            return true;
        }
    }
    return false;
}

inline std::string_view trim_cookie_whitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace detail

/// Serialize a cookie as a Set-Cookie field value.
///
/// Throws std::invalid_argument when an attribute could split or inject an HTTP
/// header. `SameSite=None` is rejected unless `Secure` is also present, matching
/// current browser requirements.
[[nodiscard]] inline std::string serialize_cookie(const Cookie& cookie) {
    if (cookie.name.empty()) {
        throw std::invalid_argument("Cookie name must not be empty");
    }
    for (const unsigned char character : cookie.name) {
        if (!detail::cookie_token_character(character)) {
            throw std::invalid_argument("Cookie name contains an invalid character");
        }
    }
    if (detail::contains_cookie_delimiter(cookie.value)) {
        throw std::invalid_argument("Cookie value contains a delimiter or control character");
    }
    if (cookie.path.empty() || cookie.path.front() != '/' ||
        detail::contains_cookie_delimiter(cookie.path)) {
        throw std::invalid_argument("Cookie path must be an absolute safe path");
    }
    if (cookie.domain && (cookie.domain->empty() ||
                          detail::contains_cookie_delimiter(*cookie.domain))) {
        throw std::invalid_argument("Cookie domain contains an invalid character");
    }
    if (cookie.same_site == SameSite::None && !cookie.secure) {
        throw std::invalid_argument("SameSite=None cookies must be Secure");
    }

    std::string result = cookie.name + "=" + cookie.value;
    result += "; Path=" + cookie.path;
    if (cookie.domain) result += "; Domain=" + *cookie.domain;
    if (cookie.max_age) result += "; Max-Age=" + std::to_string(cookie.max_age->count());
    if (cookie.secure) result += "; Secure";
    if (cookie.http_only) result += "; HttpOnly";
    switch (cookie.same_site) {
    case SameSite::Strict: result += "; SameSite=Strict"; break;
    case SameSite::Lax: result += "; SameSite=Lax"; break;
    case SameSite::None: result += "; SameSite=None"; break;
    }
    return result;
}

/// Add one Set-Cookie response field. Multiple calls intentionally create
/// multiple fields instead of merging their values.
inline void set_cookie(Response& response, const Cookie& cookie) {
    response.header("set-cookie", serialize_cookie(cookie));
}

/// Return the first request Cookie value whose name exactly matches `name`.
[[nodiscard]] inline std::optional<std::string_view>
request_cookie(const Request& request, std::string_view name) noexcept {
    for (const auto header : request.headers().get_all("cookie")) {
        std::string_view remaining = header;
        while (!remaining.empty()) {
            const auto separator = remaining.find(';');
            const auto part = detail::trim_cookie_whitespace(remaining.substr(0, separator));
            if (const auto equals = part.find('='); equals != std::string_view::npos) {
                const auto cookie_name = detail::trim_cookie_whitespace(part.substr(0, equals));
                if (cookie_name == name) return part.substr(equals + 1);
            }
            if (separator == std::string_view::npos) break;
            remaining.remove_prefix(separator + 1);
        }
    }
    return std::nullopt;
}

} // namespace novaboot::http

namespace novaboot::http3 {
using Cookie = http::Cookie;
using SameSite = http::SameSite;
using http::request_cookie;
using http::serialize_cookie;
using http::set_cookie;
} // namespace novaboot::http3
