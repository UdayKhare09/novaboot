#pragma once
#include "novaboot/context/request_context.h"
#include "novaboot/router/response_entity.h"
#include "service/auth_service.h"
#include "model/dto.h"
#include "model/app_user.h"
#include <spdlog/spdlog.h>

using namespace novaboot;
using namespace novaboot::context;
using todo_notes::service::AuthService;
using todo_notes::model::RegisterRequest;
using todo_notes::model::LoginRequest;
using todo_notes::model::LoginResponse;
using todo_notes::model::AppUser;

using namespace novaboot::annotations;

namespace todo_notes::controller {

struct [[= RestController("/public") ]] AuthController {
    AuthService& auth_service;

    explicit AuthController(AuthService& svc) : auth_service(svc) {}

    [[= PostMapping("/register") ]]
    ResponseEntity<AppUser> register_user(RegisterRequest req, RequestContext&) {
        spdlog::info("Registering user: {}", req.username);
        auto registered = auth_service.register_user(req);
        return ResponseEntity<AppUser>::status(201, registered);
    }

    [[= PostMapping("/login") ]]
    ResponseEntity<LoginResponse> login_user(LoginRequest req, RequestContext&) {
        spdlog::info("Logging in user: {}", req.username);
        auto resp = auth_service.login_user(req);
        return ResponseEntity<LoginResponse>::ok(resp);
    }
};

} // namespace todo_notes::controller
