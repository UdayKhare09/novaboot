#pragma once

#include "novaboot/di/di.h"
#include "model/user.h"
#include <vector>
#include <string>
#include <optional>

/// In-memory repository handling database operations (Spring-style Repository)
struct [[=novaboot::di::repository{}]] UserRepository {
    UserRepository() = default;

    std::optional<examples::model::User> find_by_id(int id) {
        if (id > 10) {
            return std::nullopt;
        }
        return examples::model::User::builder()
            .id(id)
            .name("John Doe")
            .email("john.doe" + std::to_string(id) + "@example.com")
            .role("ROLE_USER")
            .build();
    }

    std::vector<examples::model::User> find_all() {
        return {
            examples::model::User::builder().id(1).name("John Doe").email("john.doe@example.com").role("ROLE_USER").build(),
            examples::model::User::builder().id(2).name("Jane Smith").email("jane.smith@example.com").role("ROLE_ADMIN").build()
        };
    }
};
