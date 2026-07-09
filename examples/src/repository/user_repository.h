#pragma once

#include "novaboot/di/di.h"
#include <string>
#include <format>

/// In-memory repository handling database operations (Spring-style Repository)
struct [[=novaboot::di::repository{}]] UserRepository {
    UserRepository() = default;

    std::string find_by_id(int id) {
        return std::format(
            R"({{"id":{},"name":"John Doe","email":"john.doe{}@example.com","role":"USER"}})",
            id, id
        );
    }

    std::string find_all() {
        return R"([
            {"id":1,"name":"John Doe","email":"john.doe@example.com","role":"USER"},
            {"id":2,"name":"Jane Smith","email":"jane.smith@example.com","role":"ADMIN"}
        ])";
    }
};
