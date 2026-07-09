#pragma once

#include "novaboot/di/di.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/context/request_context.h"
#include "service/user_service.h"
#include "service/request_logger.h"
#include <string>
#include <cstdlib>

/// REST Controller mapping user APIs (Spring-style RestController)
struct [[=novaboot::di::component{}]] UserController {
    UserService& user_service;

    // Constructor injection: UserService is auto-wired
    explicit UserController(UserService& svc) : user_service(svc) {}

    void list_users(novaboot::http3::Request&, novaboot::http3::Response& res, novaboot::context::RequestContext& ctx) {
        // Access request-scoped bean via Context Injection
        auto& logger = ctx.inject<RequestLogger>();
        logger.log("Processing request: GET /api/users");

        res.status(200)
           .header("Content-Type", "application/json")
           .body(user_service.get_all_users());
    }

    void get_user(novaboot::http3::Request& req, novaboot::http3::Response& res, novaboot::context::RequestContext& ctx) {
        auto& logger = ctx.inject<RequestLogger>();
        
        // Extract route parameter
        auto id_opt = req.path_params().get_as<int>("id");
        if (!id_opt) {
            logger.log("Failed to parse user ID parameter");
            res.status(400)
               .header("Content-Type", "application/json")
               .body(R"({"error":"Invalid or missing user ID"})");
            return;
        }

        int id = *id_opt;
        logger.log("Processing request: GET /api/users/" + std::to_string(id));

        res.status(200)
           .header("Content-Type", "application/json")
           .body(user_service.get_user(id));
    }
};
