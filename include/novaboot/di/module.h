#pragma once

/// @file novaboot/di/module.h
/// Module/Configuration support for NovaBoot DI.
///
/// A Module (equivalent to Spring @Configuration) is a class annotated with
/// [[=novaboot::di::module_tag{}]] that contains [[=novaboot::di::bean{}]]-annotated
/// factory methods.
///
/// Usage:
///
///   struct [[=novaboot::di::module_tag{}]] InfraModule {
///       [[=novaboot::di::bean{}]]
///       DatabasePool make_db_pool(Config& cfg) {
///           return DatabasePool{cfg.get("db.url"), 10};
///       }
///
///       [[=novaboot::di::bean{}]]
///       [[=novaboot::di::primary{}]]
///       RedisCache make_cache(Config& cfg) {
///           return RedisCache{cfg.get("redis.url")};
///       }
///   };
///
///   // Register in RootContainer:
///   root.register_module<InfraModule>();
///
/// The module instance itself is managed by the container (singleton).
/// All [[=bean{}]] methods are called with resolved dependencies from the container.

#include "novaboot/di/attributes.h"
#include "novaboot/di/container.h"
#include <array>
#include <utility>
#include <type_traits>
#include <algorithm>

namespace novaboot::di {

namespace detail {

template<typename ModuleType, std::meta::info FactoryFn, typename Mod, typename Cont, size_t... Is>
auto call_factory_helper(Mod& mod, Cont& c, std::index_sequence<Is...>) {
    constexpr auto params_array = []() consteval {
        auto vec = std::meta::parameters_of(FactoryFn);
        std::array<std::meta::info, 16> arr = {};
        std::size_t n = vec.size() < 16 ? vec.size() : 16;
        for (std::size_t i = 0; i < n; ++i) {
            arr[i] = vec[i];
        }
        return arr;
    }();
    
    return new typename [: std::meta::return_type_of(FactoryFn) :](
        (mod.*&[: FactoryFn :])(
            c.template resolve<std::remove_cvref_t<typename [: std::meta::type_of(params_array[Is]) :]>>()...
        )
    );
}

template<typename ModuleType, std::meta::info FactoryFn>
void register_factory_bean(RootContainer& container) {
    constexpr auto ret_type = std::meta::return_type_of(FactoryFn);
    using ReturnType = typename [: ret_type :];
    
    container.register_bean<ReturnType>([](ContainerBase& c) -> ReturnType* {
        auto& mod = c.resolve<ModuleType>();
        
        constexpr auto num_params = []() consteval {
            return std::meta::parameters_of(FactoryFn).size();
        }();
        return call_factory_helper<ModuleType, FactoryFn>(mod, c, std::make_index_sequence<num_params>{});
    });
}

} // namespace detail

/// Registers all [[=bean{}]]-annotated methods from ModuleType into the container.
template<typename ModuleType>
void register_module(RootContainer& container) {
#ifdef __cpp_impl_reflection
    static_assert(novaboot::di::detail::is_module_class(^^ModuleType),
                  "register_module<T>: T must be annotated with [[=novaboot::di::module_tag{}]]");

    // Register the module itself as a singleton (so bean factories can be called on it)
    container.register_bean<ModuleType>(
        [](ContainerBase&) { return new ModuleType{}; }
    );

    // Extract factory methods in consteval and store them in std::array
    static constexpr auto factories_array = []() consteval {
        auto vec = novaboot::di::detail::get_bean_factories(^^ModuleType);
        std::array<std::meta::info, 32> arr = {};
        std::size_t n = vec.size() < 32 ? vec.size() : 32;
        for (std::size_t i = 0; i < n; ++i) {
            arr[i] = vec[i];
        }
        return std::pair{arr, n};
    }();

    // Iterate over indices at compile time using folded template expansion
    []<size_t... Is>(std::index_sequence<Is...>, RootContainer& c) {
        (
            [](RootContainer& container_ref) {
                constexpr auto factory_fn = factories_array.first[Is];
                if constexpr (factory_fn != std::meta::info{}) {
                    detail::register_factory_bean<ModuleType, factory_fn>(container_ref);
                }
            }(c), ...
        );
    }(std::make_index_sequence<32>{}, container);
#else
    static_assert(false,
        "register_module<T> requires C++26 reflection (-freflection). "
        "Use container.register_bean<T>() manually, or enable the codegen step.");
#endif
}

} // namespace novaboot::di
