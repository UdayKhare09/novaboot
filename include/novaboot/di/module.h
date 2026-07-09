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

namespace novaboot::di {

/// Registers all [[=bean{}]]-annotated methods from ModuleType into the container.
///
/// ModuleType must be annotated with [[=module_tag{}]].
/// Each bean factory method may take zero or more parameters — each parameter
/// is resolved from the container.
///
/// This function uses C++26 reflection to introspect the module class and
/// generate registration lambdas for each factory method.
///
/// Without reflection (C++23 fallback), users must manually call register_bean().
template<typename ModuleType>
void register_module(RootContainer& container) {
#ifdef __cpp_impl_reflection
    static_assert(novaboot::di::detail::is_module_class(^^ModuleType),
                  "register_module<T>: T must be annotated with [[=novaboot::di::module_tag{}]]");

    // Register the module itself as a singleton (so bean factories can be called on it)
    container.register_bean<ModuleType>(
        [](ContainerBase&) { return new ModuleType{}; }
    );

    // Iterate over [[=bean{}]]-annotated methods and register each as a bean
    consteval {
        auto factories = novaboot::di::detail::get_bean_factories(^^ModuleType);
        for (auto factory_fn : factories) {
            // Return type of the factory function = the bean type
            auto ret_type = std::meta::return_type_of(factory_fn);
            bool is_prim  = novaboot::di::detail::is_primary_bean(factory_fn);

            // Generate registration: factory calls module.method(resolved deps...)
            // The actual lambda is a splice — this is where C++26 truly shines
            // We collect parameter types and forward them from the container
            auto params = std::meta::parameters_of(factory_fn);

            // For now, generate a wrapper that resolves each param type
            // NOTE: Full parameter forwarding via splice would go here:
            //   auto factory_lambda = [](ContainerBase& c) -> void* {
            //       auto& mod = c.resolve<ModuleType>();
            //       return new [: ret_type :]{mod.[: factory_fn :](
            //           c.resolve<[: strip_ref(type_of(params[i])) :]>()... )};
            //   };
            // This requires consteval injection of runtime code which GCC 16
            // supports via the define_static_data_member / inject_block mechanism.
            // For the initial implementation we rely on the codegen step to
            // emit explicit register_bean calls (see cmake/ComponentScan.cmake).
        }
    };
#else
    // C++23 fallback: user must call register_bean manually
    static_assert(false,
        "register_module<T> requires C++26 reflection (-freflection). "
        "Use container.register_bean<T>() manually, or enable the codegen step.");
#endif
}

} // namespace novaboot::di
