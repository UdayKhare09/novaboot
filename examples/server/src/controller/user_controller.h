#pragma once

#include "novaboot/di/di.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/context/request_context.h"
#include "novaboot/router/response_entity.h"
#include "service/user_service.h"
#include "service/request_logger.h"
#include "model/user.h"
#include <string>
#include <vector>

using namespace novaboot;
using namespace novaboot::context;
using examples::model::User;

/// REST Controller mapping user APIs
struct UserController {
    UserService& user_service;

    // Constructor injection: UserService is auto-wired
    explicit UserController(UserService& svc) : user_service(svc) {}

    auto list_users(RequestContext& ctx) {
        ctx.inject<RequestLogger>().log("Processing request: GET /api/users");
        return ResponseEntity<std::vector<User>>::ok(user_service.get_all_users());
    }

    ResponseEntity<User> get_user(int id, RequestContext& ctx) {
        ctx.inject<RequestLogger>().log("Processing request: GET /api/users/" + std::to_string(id));
        return ResponseEntity<User>::ok(user_service.get_user(id));
    }

    ResponseEntity<User> create_user(
        User user,
        RequestContext& ctx
    ) {
        ctx.inject<RequestLogger>().log("Processing request: POST /api/users");
        auto saved = user_service.user_repo.save(user);
        return ResponseEntity<User>::status(201, saved);
    }

    ResponseEntity<User> update_user(
        int id,
        User user,
        RequestContext& ctx
    ) {
        ctx.inject<RequestLogger>().log("Processing request: PUT /api/users/" + std::to_string(id));
        user.id = id;
        auto saved = user_service.user_repo.save(user);
        return ResponseEntity<User>::ok(saved);
    }

    ResponseEntity<void> delete_user(int id, RequestContext& ctx) {
        ctx.inject<RequestLogger>().log("Processing request: DELETE /api/users/" + std::to_string(id));
        user_service.user_repo.delete_by_id(id);
        return ResponseEntity<void>::noContent();
    }

    ResponseEntity<User> patch_user(
        int id,
        User user,
        RequestContext& ctx
    ) {
        ctx.inject<RequestLogger>().log("Processing request: PATCH /api/users/" + std::to_string(id));
        user.id = id;
        return ResponseEntity<User>::ok(user);
    }
};

