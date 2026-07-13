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

/// REST Controller mapping user APIs
struct UserController {
    UserService& user_service;

    // Constructor injection: UserService is auto-wired
    explicit UserController(UserService& svc) : user_service(svc) {}

    auto list_users(novaboot::context::RequestContext& ctx) {
        ctx.inject<RequestLogger>().log("Processing request: GET /api/users");
        return novaboot::ResponseEntity<std::vector<examples::model::User>>::ok(user_service.get_all_users());
    }

    novaboot::ResponseEntity<examples::model::User> get_user(int id, novaboot::context::RequestContext& ctx) {
        ctx.inject<RequestLogger>().log("Processing request: GET /api/users/" + std::to_string(id));
        return novaboot::ResponseEntity<examples::model::User>::ok(user_service.get_user(id));
    }

    novaboot::ResponseEntity<examples::model::User> create_user(
        examples::model::User user,
        novaboot::context::RequestContext& ctx
    ) {
        ctx.inject<RequestLogger>().log("Processing request: POST /api/users");
        auto saved = user_service.user_repo.save(user);
        return novaboot::ResponseEntity<examples::model::User>::status(201, saved);
    }

    novaboot::ResponseEntity<examples::model::User> update_user(
        int id,
        examples::model::User user,
        novaboot::context::RequestContext& ctx
    ) {
        ctx.inject<RequestLogger>().log("Processing request: PUT /api/users/" + std::to_string(id));
        user.id = id;
        auto saved = user_service.user_repo.save(user);
        return novaboot::ResponseEntity<examples::model::User>::ok(saved);
    }

    novaboot::ResponseEntity<void> delete_user(int id, novaboot::context::RequestContext& ctx) {
        ctx.inject<RequestLogger>().log("Processing request: DELETE /api/users/" + std::to_string(id));
        user_service.user_repo.delete_by_id(id);
        return novaboot::ResponseEntity<void>::noContent();
    }

    novaboot::ResponseEntity<examples::model::User> patch_user(
        int id,
        examples::model::User user,
        novaboot::context::RequestContext& ctx
    ) {
        ctx.inject<RequestLogger>().log("Processing request: PATCH /api/users/" + std::to_string(id));
        user.id = id;
        return novaboot::ResponseEntity<examples::model::User>::ok(user);
    }
};
