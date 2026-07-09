#pragma once
#include "novaboot/di/di.h"
#include <spdlog/spdlog.h>
#include <string>
#include <format>
#include <vector>

// ─── Domain types ─────────────────────────────────────────────────────────────

/// Simple in-memory user store (repository stereotype)
struct [[=novaboot::di::repository{}]] UserRepository {
    std::string find(int id) {
        return std::format(R"({{"id":{},"name":"User {}","email":"user{}@example.com"}})",
                           id, id, id);
    }
};

/// Business logic service (service stereotype)
struct [[=novaboot::di::service{}]] UserService {
    UserRepository& repo_;

    /// Constructor injection: DI container detects the single constructor
    explicit UserService(UserRepository& repo) : repo_(repo) {}

    std::string get_user(int id) { return repo_.find(id); }
    std::string list_users()     { return R"([{"id":1},{"id":2},{"id":3}])"; }

    [[=novaboot::di::post_construct{}]]
    void on_start() { spdlog::info("UserService ready"); }

    [[=novaboot::di::pre_destroy{}]]
    void on_stop()  { spdlog::info("UserService shutting down"); }
};

/// Request-scoped logger (one per HTTP request)
struct [[=novaboot::di::component{}]]
       [[=novaboot::di::scoped{novaboot::di::Scope::Request}]]
RequestLogger {
    std::vector<std::string> events;
    void log(std::string_view msg) { events.emplace_back(msg); }
};
