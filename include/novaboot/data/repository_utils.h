#pragma once

#include <stdexcept>

namespace novaboot::data::detail {

template<typename E>
auto get_entity_id(const E& entity) {
    if constexpr (requires { entity.id; }) {
        return entity.id;
    } else {
        throw std::runtime_error("Entity has no default 'id' field.");
    }
}

} // namespace novaboot::data::detail
