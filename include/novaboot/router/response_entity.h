#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace novaboot {

/// Spring Boot-style ResponseEntity class to wrap HTTP status, headers, and body DTO.
template<typename T>
class ResponseEntity {
public:
    ResponseEntity(T body, int status = 200)
        : body_(std::move(body)), status_(status) {}

    /// Add a custom header
    ResponseEntity& header(std::string name, std::string value) {
        headers_.push_back({std::move(name), std::move(value)});
        return *this;
    }

    // Static builders
    static ResponseEntity<T> ok(T body) {
        return ResponseEntity<T>(std::move(body), 200);
    }

    static ResponseEntity<T> status(int status_code, T body) {
        return ResponseEntity<T>(std::move(body), status_code);
    }

    [[nodiscard]] const T& body() const noexcept { return body_; }
    [[nodiscard]] int status_code() const noexcept { return status_; }
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& headers() const noexcept {
        return headers_;
    }

private:
    T body_;
    int status_ = 200;
    std::vector<std::pair<std::string, std::string>> headers_;
};

/// ResponseEntity specialization for empty/void bodies (e.g. 204 No Content)
template<>
class ResponseEntity<void> {
public:
    explicit ResponseEntity(int status = 200) : status_(status) {}

    ResponseEntity& header(std::string name, std::string value) {
        headers_.push_back({std::move(name), std::move(value)});
        return *this;
    }

    static ResponseEntity<void> ok() {
        return ResponseEntity<void>(200);
    }

    static ResponseEntity<void> status(int status_code) {
        return ResponseEntity<void>(status_code);
    }

    static ResponseEntity<void> noContent() {
        return ResponseEntity<void>(204);
    }

    [[nodiscard]] int status_code() const noexcept { return status_; }
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& headers() const noexcept {
        return headers_;
    }

private:
    int status_ = 200;
    std::vector<std::pair<std::string, std::string>> headers_;
};

} // namespace novaboot
