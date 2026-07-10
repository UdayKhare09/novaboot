#pragma once
#include <cstddef>
#ifdef __cpp_impl_reflection
#  include <meta>
#endif

namespace novaboot::web {

struct get {
    char path[128] = {};
    consteval get(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct post {
    char path[128] = {};
    consteval post(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct put {
    char path[128] = {};
    consteval put(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct del {
    char path[128] = {};
    consteval del(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct patch {
    char path[128] = {};
    consteval patch(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct head {
    char path[128] = {};
    consteval head(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct options {
    char path[128] = {};
    consteval options(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct any {
    char path[128] = {};
    consteval any(const char* p) noexcept {
        for (std::size_t i = 0; i < 127 && p[i]; ++i) path[i] = p[i];
    }
    consteval const char* str() const noexcept { return path; }
};

struct request_body {};

struct controller {
    char path[64] = {};

    consteval controller() = default;
    consteval controller(const char* s) noexcept {
        for (std::size_t i = 0; i < 63u && s[i]; ++i)
            path[i] = s[i];
    }
};

struct rest_controller {
    char path[64] = {};

    consteval rest_controller() = default;
    consteval rest_controller(const char* s) noexcept {
        for (std::size_t i = 0; i < 63u && s[i]; ++i)
            path[i] = s[i];
    }
};

/// Declarative HTTP/3 REST client annotation.
/// Place on an abstract class to mark it as a Feign-style remote client.
/// Individual methods should be annotated with [[=novaboot::web::get{...}]] etc.
///
/// Example:
///   [[=novaboot::web::rest_client{"https://api.example.com:4433"}]]
///   class UserServiceClient {
///       [[=novaboot::web::get{"/api/users/{id}"}]]
///       novaboot::ResponseEntity<User> get_user(int id);
///   };
struct rest_client {
    char url[256] = {};  ///< Full URL including scheme + host + port

    consteval rest_client() = default;
    consteval rest_client(const char* u) noexcept {
        for (std::size_t i = 0; i < 255u && u[i]; ++i)
            url[i] = u[i];
    }
    consteval const char* str() const noexcept { return url; }
};

struct controller_advice {};


struct exception_handler {
#ifdef __cpp_impl_reflection
    std::meta::info exception_type;
#endif
};

} // namespace novaboot::web
