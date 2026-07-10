#pragma once

#include "novaboot/data/data_attributes.h"
#include <stdexcept>

#ifdef __cpp_impl_reflection
#include <meta>
#endif

namespace novaboot::data::detail {

template<typename E>
auto get_entity_id(const E& entity) {
#ifdef __cpp_impl_reflection
    constexpr auto member_ptr = []() consteval {
        constexpr auto ctx = std::meta::access_context::current();
        for (auto m : std::meta::members_of(^^E, ctx)) {
            if (!std::meta::annotations_of_with_type(m, ^^novaboot::data::id).empty()) {
                return m;
            }
        }
        return std::meta::info{};
    }();

    if constexpr (member_ptr != std::meta::info{}) {
        return entity.*&[:member_ptr:];
    } else {
        if constexpr (requires { entity.id; }) {
            return entity.id;
        } else {
            throw std::runtime_error("Entity has no field annotated with [[=data::id{}]] and no default 'id' field.");
        }
    }
#else
    if constexpr (requires { entity.id; }) {
        return entity.id;
    } else {
        throw std::runtime_error("Entity has no default 'id' field.");
    }
#endif
}

} // namespace novaboot::data::detail
