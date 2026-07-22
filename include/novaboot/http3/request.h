#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "novaboot/http3/header_map.h"
#include "novaboot/router/path_params.h"

namespace novaboot::http {

using HeaderMap = http3::HeaderMap;

/// HTTP request representation.
///
/// Populated by nghttp3 callbacks as headers and body arrive.
/// All string data is owned by this object.
class Request {
public:
    Request() = default;

    // ─── HTTP/3 pseudo-headers ───────────────────────────────────────
    /// HTTP method (GET, POST, etc.)
    [[nodiscard]] std::string_view method() const noexcept { return method_; }
    void set_method(std::string_view m) { method_ = std::string(m); }

    /// Request path (e.g., "/api/users/42")
    [[nodiscard]] std::string_view path() const noexcept { return path_; }
    void set_path(std::string_view p) { path_ = std::string(p); }

    /// Scheme (https)
    [[nodiscard]] std::string_view scheme() const noexcept { return scheme_; }
    void set_scheme(std::string_view s) { scheme_ = std::string(s); }

    /// Authority (host:port)
    [[nodiscard]] std::string_view authority() const noexcept {
        return authority_;
    }
    void set_authority(std::string_view a) { authority_ = std::string(a); }

    /// Direct socket peer IP address, supplied by the server transport. It is
    /// never populated from client-supplied forwarding headers.
    [[nodiscard]] std::string_view peer_address() const noexcept { return peer_address_; }
    void set_peer_address(std::string_view address) {
        peer_address_ = std::string(address);
        client_address_ = peer_address_;
    }

    /// Effective client IP address. It initially equals peer_address(); a
    /// trusted-proxy middleware may replace it only after CIDR verification.
    [[nodiscard]] std::string_view client_address() const noexcept { return client_address_; }
    void set_client_address(std::string_view address) { client_address_ = std::string(address); }

    // ─── Headers ─────────────────────────────────────────────────────
    [[nodiscard]] const HeaderMap& headers() const noexcept {
        return headers_;
    }
    [[nodiscard]] HeaderMap& headers() noexcept { return headers_; }

    /// Convenience: get a specific header
    [[nodiscard]] std::optional<std::string_view>
    header(std::string_view name) const {
        return headers_.get(name);
    }

    // ─── Body ────────────────────────────────────────────────────────
    [[nodiscard]] std::string_view body() const noexcept { return body_; }
    void append_body(const uint8_t* data, std::size_t len) {
        body_.append(reinterpret_cast<const char*>(data), len);
    }

    /// Content-Length (parsed from header, or body size)
    [[nodiscard]] std::size_t content_length() const noexcept {
        return body_.size();
    }

    // ─── Path parameters (populated by router) ───────────────────────
    [[nodiscard]] const router::PathParams& path_params() const noexcept {
        return path_params_;
    }
    [[nodiscard]] router::PathParams& path_params() noexcept {
        return path_params_;
    }

    // ─── Query parameters (lazy-parsed) ──────────────────────────────
    /// Get a query parameter by name
    [[nodiscard]] std::optional<std::string_view>
    query_param(std::string_view name) const;

    /// Get the raw query string
    [[nodiscard]] std::string_view query_string() const noexcept;

    // ─── State tracking ──────────────────────────────────────────────
    [[nodiscard]] bool headers_complete() const noexcept {
        return headers_complete_;
    }
    void mark_headers_complete() noexcept { headers_complete_ = true; }

    [[nodiscard]] bool body_complete() const noexcept {
        return body_complete_;
    }
    void mark_body_complete() noexcept { body_complete_ = true; }

private:
    std::string method_;
    std::string path_;
    std::string scheme_;
    std::string authority_;
    std::string peer_address_;
    std::string client_address_;

    HeaderMap headers_;
    std::string body_;

    router::PathParams path_params_;

    bool headers_complete_ = false;
    bool body_complete_    = false;
};

} // namespace novaboot::http

namespace novaboot::http3 {
using Request = http::Request;
}
