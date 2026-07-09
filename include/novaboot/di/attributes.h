#pragma once

/// @file novaboot/di/attributes.h
/// Annotation tags for NovaBoot DI — Spring Boot-style stereotypes.
///
/// REQUIRES: GCC 16+ with -std=c++26 -freflection
///
/// Annotation syntax (C++26 P3385): [[=tag_type{}]]
/// All annotation types MUST be structural types (no std::string_view members).
///
/// ─── Usage ────────────────────────────────────────────────────────────────────
///
///   struct [[=novaboot::di::component{}]] UserService {
///       explicit UserService(UserRepository& repo);
///       [[=novaboot::di::post_construct{}]] void init();
///       [[=novaboot::di::pre_destroy{}]]    void shutdown();
///   };
///
///   struct [[=novaboot::di::service{}]] [[=novaboot::di::scoped{novaboot::di::Scope::Request}]]
///   RequestLogger { ... };
///
///   // Named qualifier via type tag (recommended)
///   namespace novaboot::di::qual { struct redis {}; }
///   struct [[=novaboot::di::component{}]] [[=novaboot::di::qual::redis{}]]
///   RedisCache : public Cache { ... };
///
///   // String qualifier (runtime lookup)
///   struct [[=novaboot::di::component{}]] [[=novaboot::di::named{"redis"}]]
///   RedisCache2 : public Cache { ... };
///
///   // Module with bean factories
///   struct [[=novaboot::di::module_tag{}]] InfraModule {
///       [[=novaboot::di::bean{}]] DatabasePool make_db(Config& cfg);
///   };

#include "novaboot/di/scope.h"

// Include <meta> BEFORE any namespace to avoid pollution of novaboot::di::std
#ifdef __cpp_impl_reflection
#  include <meta>
#endif

#include <cstdint>
#include <cstring>

namespace novaboot::di {

// ─────────────────────────────────────────────────────────────────────────────
// Stereotype annotation tags
// ─────────────────────────────────────────────────────────────────────────────

struct component    {};  ///< Marks a class as a DI-managed bean (Singleton by default)
struct service      {};  ///< Semantic alias — service layer bean
struct repository   {};  ///< Semantic alias — data access layer bean
struct configuration{};  ///< Semantic alias — configuration/infrastructure bean

// ─────────────────────────────────────────────────────────────────────────────
// Module / Bean
// ─────────────────────────────────────────────────────────────────────────────

struct module_tag   {};  ///< Marks a class as a configuration module
struct bean         {};  ///< Marks a member fn as a bean factory inside a module

// ─────────────────────────────────────────────────────────────────────────────
// Scope override
// ─────────────────────────────────────────────────────────────────────────────

/// Override the default (Singleton) scope of a component.
struct scoped {
    Scope value = Scope::Singleton;
};

// ─────────────────────────────────────────────────────────────────────────────
// Qualifiers
// ─────────────────────────────────────────────────────────────────────────────

/// String-based named qualifier (runtime-accessible, structural fixed-char-array).
/// Annotation types must be structural — std::string_view is NOT structural.
struct named {
    char value[64] = {};

    consteval named(const char* s) noexcept {
        for (std::size_t i = 0; i < 63u && s[i]; ++i)
            value[i] = s[i];
    }

    consteval bool operator==(const named& o) const noexcept {
        for (std::size_t i = 0; i < 64u; ++i)
            if (value[i] != o.value[i]) return false;
        return true;
    }

    consteval const char* str() const noexcept { return value; }
};

/// Primary bean — preferred when injecting an interface with multiple implementations
/// and no explicit qualifier is provided.
struct primary {};

/// User-defined qualifier tag namespace.
/// Users add their own tag types here:
///   namespace novaboot::di::qual { struct redis {}; struct memcached {}; }
namespace qual {}

// ─────────────────────────────────────────────────────────────────────────────
// Behaviour modifiers
// ─────────────────────────────────────────────────────────────────────────────

struct lazy {};  ///< Defer construction to first resolve<T>()

/// Async initialization — factory function returns std::future<T>.
struct async_init {
    std::uint32_t timeout_ms = 30'000u;  ///< 0 = wait forever
};

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle annotations (on member functions)
// ─────────────────────────────────────────────────────────────────────────────

struct post_construct {};  ///< Called after construction + dep injection
struct pre_destroy    {};  ///< Called before the bean is destroyed

// ─────────────────────────────────────────────────────────────────────────────
// Constructor disambiguation
// ─────────────────────────────────────────────────────────────────────────────

struct inject {};  ///< Choose this ctor when multiple user ctors exist

} // namespace novaboot::di

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time annotation helpers (reflection-only, not inside novaboot::di
// namespace to avoid std:: ambiguity with #include <meta>)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __cpp_impl_reflection

namespace novaboot::di::detail {

/// True if cls has any DI stereotype annotation.
consteval bool is_managed_component(std::meta::info cls) noexcept {
    return !std::meta::annotations_of_with_type(cls, ^^novaboot::di::component).empty()
        || !std::meta::annotations_of_with_type(cls, ^^novaboot::di::service).empty()
        || !std::meta::annotations_of_with_type(cls, ^^novaboot::di::repository).empty()
        || !std::meta::annotations_of_with_type(cls, ^^novaboot::di::configuration).empty();
}

/// True if cls has [[=module_tag{}]].
consteval bool is_module_class(std::meta::info cls) noexcept {
    return !std::meta::annotations_of_with_type(cls, ^^novaboot::di::module_tag).empty();
}

/// Returns Scope of cls (default: Singleton).
consteval Scope get_scope(std::meta::info cls) noexcept {
    auto annots = std::meta::annotations_of_with_type(cls, ^^novaboot::di::scoped);
    if (annots.empty()) return Scope::Singleton;
    return std::meta::extract<novaboot::di::scoped>(annots[0]).value;
}

/// True if cls has [[=primary{}]].
consteval bool is_primary_bean(std::meta::info cls) noexcept {
    return !std::meta::annotations_of_with_type(cls, ^^novaboot::di::primary).empty();
}

/// True if cls has [[=lazy{}]].
consteval bool is_lazy_bean(std::meta::info cls) noexcept {
    return !std::meta::annotations_of_with_type(cls, ^^novaboot::di::lazy).empty();
}

/// True if cls has [[=async_init{...}]].
consteval bool is_async_bean(std::meta::info cls) noexcept {
    return !std::meta::annotations_of_with_type(cls, ^^novaboot::di::async_init).empty();
}

/// Returns async timeout_ms or 0 if not annotated.
consteval std::uint32_t get_async_timeout(std::meta::info cls) noexcept {
    auto annots = std::meta::annotations_of_with_type(cls, ^^novaboot::di::async_init);
    if (annots.empty()) return 0u;
    return std::meta::extract<novaboot::di::async_init>(annots[0]).timeout_ms;
}

/// True if cls has [[=named{"..."}]] matching expected.
consteval bool has_named_qualifier(std::meta::info cls,
                                   const novaboot::di::named& expected) noexcept {
    auto annots = std::meta::annotations_of_with_type(cls, ^^novaboot::di::named);
    for (auto a : annots) {
        if (std::meta::extract<novaboot::di::named>(a) == expected) return true;
    }
    return false;
}

/// Returns the first named qualifier string (empty string if none).
consteval const char* get_named_qualifier(std::meta::info cls) noexcept {
    auto annots = std::meta::annotations_of_with_type(cls, ^^novaboot::di::named);
    if (annots.empty()) return "";
    return std::meta::extract<novaboot::di::named>(annots[0]).str();
}

/// Finds the constructor to use for injection.
/// Priority: [[=inject{}]] annotated > single user ctor > default ctor
/// Throws std::meta::exception if ambiguous.
consteval std::meta::info find_inject_ctor(std::meta::info cls) {
    constexpr auto ctx = std::meta::access_context::current();
    std::meta::info inject_ctor{};
    std::meta::info default_ctor{};
    std::size_t user_ctor_count = 0;
    std::meta::info last_user_ctor{};

    for (auto m : std::meta::members_of(cls, ctx)) {
        if (!std::meta::is_constructor(m)) continue;
        if (std::meta::is_copy_constructor(m)) continue;
        if (std::meta::is_move_constructor(m)) continue;

        if (std::meta::is_default_constructor(m)) {
            default_ctor = m;
        } else {
            ++user_ctor_count;
            last_user_ctor = m;
            if (!std::meta::annotations_of_with_type(m, ^^novaboot::di::inject).empty())
                inject_ctor = m;
        }
    }

    if (inject_ctor    != std::meta::info{}) return inject_ctor;
    if (user_ctor_count == 1)                return last_user_ctor;
    if (default_ctor   != std::meta::info{}) return default_ctor;

    throw std::meta::exception(
        u8"novaboot::di: No injectable constructor found. "
        u8"Annotate the constructor with [[=novaboot::di::inject{}]].",
        cls
    );
}

/// Collect dependency types from a constructor's parameter list (refs + const stripped).
consteval std::vector<std::meta::info> collect_dep_types(std::meta::info ctor) {
    std::vector<std::meta::info> deps;
    for (auto param : std::meta::parameters_of(ctor)) {
        auto t = std::meta::type_of(param);
        if (std::meta::is_reference_type(t)) t = std::meta::remove_reference(t);
        if (std::meta::is_const(t))          t = std::meta::remove_const(t);
        deps.push_back(t);
    }
    return deps;
}

/// Find [[=post_construct{}]]-annotated member functions.
consteval std::vector<std::meta::info> get_post_construct_fns(std::meta::info cls) {
    constexpr auto ctx = std::meta::access_context::current();
    std::vector<std::meta::info> result;
    for (auto m : std::meta::members_of(cls, ctx)) {
        if (std::meta::is_function(m) && !std::meta::is_constructor(m)
            && !std::meta::is_destructor(m)
            && !std::meta::annotations_of_with_type(m, ^^novaboot::di::post_construct).empty())
            result.push_back(m);
    }
    return result;
}

/// Find [[=pre_destroy{}]]-annotated member functions.
consteval std::vector<std::meta::info> get_pre_destroy_fns(std::meta::info cls) {
    constexpr auto ctx = std::meta::access_context::current();
    std::vector<std::meta::info> result;
    for (auto m : std::meta::members_of(cls, ctx)) {
        if (std::meta::is_function(m) && !std::meta::is_constructor(m)
            && !std::meta::is_destructor(m)
            && !std::meta::annotations_of_with_type(m, ^^novaboot::di::pre_destroy).empty())
            result.push_back(m);
    }
    return result;
}

/// Find [[=bean{}]]-annotated member functions inside a module class.
consteval std::vector<std::meta::info> get_bean_factories(std::meta::info module_cls) {
    constexpr auto ctx = std::meta::access_context::current();
    std::vector<std::meta::info> result;
    for (auto m : std::meta::members_of(module_cls, ctx)) {
        if (std::meta::is_function(m) && !std::meta::is_constructor(m)
            && !std::meta::is_destructor(m)
            && !std::meta::annotations_of_with_type(m, ^^novaboot::di::bean).empty())
            result.push_back(m);
    }
    return result;
}

} // namespace novaboot::di::detail

#endif // __cpp_impl_reflection
