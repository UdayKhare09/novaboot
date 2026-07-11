#pragma once

#include "novaboot/di/di.h"
#include "repository/user_repository.h"
#include "model/user.h"
#include "exception/user_not_found_exception.h"
#include <spdlog/spdlog.h>
#include <vector>
#include <optional>

/// Business logic implementation (Spring-style Service)
struct [[=novaboot::di::service{}]] UserService {
    UserRepository& user_repo;

    // Constructor injection: DI container automatically resolves and injects UserRepository
    explicit UserService(UserRepository& repo) : user_repo(repo) {}

    examples::model::User get_user(int id) {
        auto opt = user_repo.find_by_id(id);
        if (!opt) {
            throw examples::exception::UserNotFoundException(id);
        }
        return *opt;
    }

    std::vector<examples::model::User> get_all_users() {
        return user_repo.find_all();
    }

    [[=novaboot::di::post_construct{}]]
    void init() {
        spdlog::info("UserService initialized successfully with UserRepository auto-wired.");
    }

    [[=novaboot::di::pre_destroy{}]]
    void cleanup() {
        spdlog::info("UserService shutting down.");
    }
};
