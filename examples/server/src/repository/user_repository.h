#pragma once

#include "model/user.h"
#include <vector>
#include <optional>
#include <mutex>
#include <algorithm>

#include "novaboot/novaboot.h"

using namespace novaboot::annotations;
using examples::model::User;

struct [[= Repository() ]] UserRepository {
private:
    std::vector<User> users_;
    std::mutex mutex_;
    int next_id_ = 1;

public:
    UserRepository() {
        users_.push_back(User{ .id = 1, .name = "Alice", .email = "alice@example.com", .role = "ROLE_ADMIN" });
        users_.push_back(User{ .id = 2, .name = "Bob", .email = "bob@example.com", .role = "ROLE_USER" });
        next_id_ = 3;
    }

    std::optional<User> find_by_id(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(users_.begin(), users_.end(), [id](const User& u) { return u.id == id; });
        if (it != users_.end()) {
            return *it;
        }
        return std::nullopt;
    }

    std::vector<User> find_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        return users_;
    }

    User save(User u) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (u.id == 0) {
            u.id = next_id_++;
            users_.push_back(u);
            return u;
        } else {
            auto it = std::find_if(users_.begin(), users_.end(), [id = u.id](const User& existing) { return existing.id == id; });
            if (it != users_.end()) {
                *it = u;
                return u;
            } else {
                users_.push_back(u);
                return u;
            }
        }
    }

    void delete_by_id(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        users_.erase(
            std::remove_if(users_.begin(), users_.end(), [id](const User& u) { return u.id == id; }),
            users_.end()
        );
    }
};
