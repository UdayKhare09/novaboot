#pragma once

#include <cstdint>
#include <algorithm>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

#include "novaboot/http3/header_map.h"

namespace novaboot::http {

namespace sse { class Channel; }

using HeaderMap = http3::HeaderMap;

/// HTTP response builder with fluent API.
///
/// Usage:
///   res.status(200)
///      .header("Content-Type", "application/json")
///      .body("{\"message\": \"hello\"}");
class Response {
public:
    Response() = default;

    // ─── Fluent API ──────────────────────────────────────────────────
    /// Set the HTTP status code
    Response& status(int code) {
        status_code_ = code;
        return *this;
    }

    /// Add a header
    Response& header(std::string_view name, std::string_view value) {
        headers_.add(name, value);
        return *this;
    }

    /// Set the response body from a null-terminated string literal
    Response& body(const char* data) {
        body_ = data;
        return *this;
    }

    /// Set the response body from a string
    Response& body(std::string_view data) {
        body_ = std::string(data);
        return *this;
    }

    /// Set the response body from raw bytes
    Response& body(const std::uint8_t* data, std::size_t len) {
        body_.assign(reinterpret_cast<const char*>(data), len);
        return *this;
    }

    /// Set the response body (move)
    Response& body(std::string&& data) {
        body_ = std::move(data);
        return *this;
    }

    // ─── JSON convenience ────────────────────────────────────────────
    /// Set body as JSON with appropriate content-type
    Response& json(std::string_view json_str) {
        headers_.set("content-type", "application/json");
        body_ = std::string(json_str);
        return *this;
    }

    /// Set a UTF-8 plain-text representation.
    Response& text(std::string_view text) {
        headers_.set("content-type", "text/plain; charset=utf-8");
        body_ = std::string(text);
        return *this;
    }

    /// Mark the current body as a download. Unsafe client-supplied filename
    /// characters are replaced, preventing header injection and path use.
    Response& download(std::string_view filename,
                       std::string_view content_type = "application/octet-stream") {
        std::string safe_name;
        safe_name.reserve(filename.size());
        for (const unsigned char character : filename) {
            if (character >= 0x20 && character != '"' && character != '\\' &&
                character != '/' && character != ':') {
                safe_name.push_back(static_cast<char>(character));
            } else {
                safe_name.push_back('_');
            }
        }
        if (safe_name.empty()) safe_name = "download";
        headers_.set("content-type", content_type);
        headers_.set("content-disposition", "attachment; filename=\"" + safe_name + "\"");
        return *this;
    }

    /// Attach a persistent Server-Sent Events producer. The selected transport
    /// owns the stream lifetime and drains the bounded channel.
    Response& event_stream(std::shared_ptr<sse::Channel> channel) {
        sse_channel_ = std::move(channel);
        return *this;
    }

    [[nodiscard]] const std::shared_ptr<sse::Channel>& event_stream() const noexcept {
        return sse_channel_;
    }

    // ─── Accessors ───────────────────────────────────────────────────
    [[nodiscard]] int status_code() const noexcept { return status_code_; }

    [[nodiscard]] const HeaderMap& headers() const noexcept {
        return headers_;
    }
    [[nodiscard]] HeaderMap& headers() noexcept { return headers_; }

    [[nodiscard]] std::string_view body_data() const noexcept {
        return body_;
    }

    [[nodiscard]] const std::string& body_str() const noexcept {
        return body_;
    }

    [[nodiscard]] std::size_t body_size() const noexcept {
        return body_.size();
    }

    // ─── State ───────────────────────────────────────────────────────
    [[nodiscard]] bool is_submitted() const noexcept { return submitted_; }
    void mark_submitted() noexcept { submitted_ = true; }

    /// How much of the body has been sent to the wire
    [[nodiscard]] std::size_t bytes_sent() const noexcept {
        return bytes_sent_;
    }
    void add_bytes_sent(std::size_t n) noexcept { bytes_sent_ += n; }

    /// How much of the body has been provided to nghttp3
    [[nodiscard]] std::size_t bytes_provided() const noexcept {
        return bytes_provided_;
    }
    void add_bytes_provided(std::size_t n) noexcept { bytes_provided_ += n; }

    [[nodiscard]] bool is_body_complete() const noexcept {
        return bytes_sent_ >= body_.size();
    }

private:
    int         status_code_ = 200;
    HeaderMap   headers_;
    std::string body_;
    std::shared_ptr<sse::Channel> sse_channel_;
    bool        submitted_  = false;
    std::size_t bytes_sent_ = 0;
    std::size_t bytes_provided_ = 0;
};

} // namespace novaboot::http

namespace novaboot::http3 {
using Response = http::Response;
}
