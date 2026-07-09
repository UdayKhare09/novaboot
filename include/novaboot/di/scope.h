#pragma once

/// @file novaboot/di/scope.h
/// Bean scope definitions for the NovaBoot DI container.
///
/// REQUIRES: GCC 16+ with -freflection -std=c++26
///
/// Scopes control how long a bean lives and which container owns it:
///
///   Singleton  — One instance for the entire application lifetime.
///                Owned by RootContainer. Shared read-only across shards.
///
///   Prototype  — A fresh instance on every resolve<T>() call.
///                Caller owns it (returned as std::unique_ptr).
///
///   Connection — One instance per QUIC connection.
///                Owned by ConnectionContainer. Destroyed with the connection.
///
///   Request    — One instance per HTTP/3 stream.
///                Owned by RequestContainer. Arena-allocated, destroyed with stream.

namespace novaboot::di {

/// Bean scope enumeration
enum class Scope : unsigned char {
    Singleton  = 0,  ///< Application-lifetime, one instance
    Prototype  = 1,  ///< New instance per resolve call
    Connection = 2,  ///< One instance per QUIC connection
    Request    = 3,  ///< One instance per HTTP/3 request stream
};

/// Returns a human-readable name for a scope (useful in error messages)
consteval const char* scope_name(Scope s) noexcept {
    switch (s) {
        case Scope::Singleton:  return "Singleton";
        case Scope::Prototype:  return "Prototype";
        case Scope::Connection: return "Connection";
        case Scope::Request:    return "Request";
    }
    return "Unknown";
}

/// Traits for each scope
template<Scope S>
struct ScopeTraits {
    /// Whether beans of this scope are thread-safe to read concurrently
    static constexpr bool is_shared    = false;
    /// Whether beans of this scope are arena-allocated (fast, bulk-freed)
    static constexpr bool is_arena     = false;
    /// Human-readable name
    static constexpr const char* name  = scope_name(S);
};

template<>
struct ScopeTraits<Scope::Singleton> {
    static constexpr bool is_shared   = true;   // immutable after build()
    static constexpr bool is_arena    = false;
    static constexpr const char* name = "Singleton";
};

template<>
struct ScopeTraits<Scope::Prototype> {
    static constexpr bool is_shared   = false;
    static constexpr bool is_arena    = false;
    static constexpr const char* name = "Prototype";
};

template<>
struct ScopeTraits<Scope::Connection> {
    static constexpr bool is_shared   = false;
    static constexpr bool is_arena    = true;
    static constexpr const char* name = "Connection";
};

template<>
struct ScopeTraits<Scope::Request> {
    static constexpr bool is_shared   = false;
    static constexpr bool is_arena    = true;
    static constexpr const char* name = "Request";
};

} // namespace novaboot::di
