#pragma once

#include "novaboot/di/di.h"
#include "repository/user_repository.h"
#include "model/user.h"
#include "exception/user_not_found_exception.h"
#include <spdlog/spdlog.h>
#include <vector>
#include <optional>

using examples::model::User;
using examples::exception::UserNotFoundException;

/// Business logic implementation (Spring-style Service)
struct UserService {
    UserRepository& user_repo;

    // Constructor injection: DI container automatically resolves and injects UserRepository
    explicit UserService(UserRepository& repo) : user_repo(repo) {}

    User get_user(int id) {
        auto opt = user_repo.find_by_id(id);
        if (!opt) {
            throw UserNotFoundException(id);
        }
        return *opt;
    }

    std::vector<User> get_all_users() {
        return user_repo.find_all();
    }

    void init() {
        spdlog::info("UserService initialized successfully with UserRepository auto-wired.");
    }

    void cleanup() {
        spdlog::info("UserService shutting down.");
    }
};

