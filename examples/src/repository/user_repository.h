#pragma once

#include "novaboot/di/di.h"
#include "model/user.h"
#include <vector>
#include <string>

/// In-memory repository handling database operations (Spring-style Repository)
struct [[=novaboot::di::repository{}]] UserRepository {
    UserRepository() = default;

    examples::model::User find_by_id(int id) {
        return {id, "John Doe", "john.doe" + std::to_string(id) + "@example.com", "USER"};
    }

    std::vector<examples::model::User> find_all() {
        return {
            {1, "John Doe", "john.doe@example.com", "USER"},
            {2, "Jane Smith", "jane.smith@example.com", "ADMIN"}
        };
    }
};
