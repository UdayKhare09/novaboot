#pragma once
#include <cstddef>

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

} // namespace novaboot::web
