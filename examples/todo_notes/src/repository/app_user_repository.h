#pragma once

#include "model/app_user.h"
#include <vector>
#include <optional>
#include <mutex>
#include <algorithm>

#include "novaboot/novaboot.h"

using namespace novaboot::annotations;
using todo_notes::model::AppUser;

struct [[= Repository() ]] AppUserRepository {
private:
    std::vector<AppUser> users_;
    std::mutex mutex_;

public:
    AppUserRepository() = default;

    std::optional<AppUser> find_by_username(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(users_.begin(), users_.end(), [&](const AppUser& u) { return u.username == username; });
        if (it != users_.end()) return *it;
        return std::nullopt;
    }

    std::optional<AppUser> find_by_email(const std::string& email) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(users_.begin(), users_.end(), [&](const AppUser& u) { return u.email == email; });
        if (it != users_.end()) return *it;
        return std::nullopt;
    }

    std::optional<AppUser> find_by_id(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(users_.begin(), users_.end(), [&](const AppUser& u) { return u.id == id; });
        if (it != users_.end()) return *it;
        return std::nullopt;
    }

    AppUser save(const AppUser& user) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(users_.begin(), users_.end(), [&](const AppUser& u) { return u.id == user.id; });
        if (it != users_.end()) {
            *it = user;
        } else {
            users_.push_back(user);
        }
        return user;
    }
};
