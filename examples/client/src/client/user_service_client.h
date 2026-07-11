#pragma once

/// Example: Declarative HTTP REST client using NovaBoot annotations.
///
/// This mirrors the Spring Boot @FeignClient pattern exactly.
/// The [[=novaboot::web::rest_client{...}]] annotation marks this as a
/// remote HTTP client interface. The novaboot-scanner tool reads this file
/// at build time and generates "user_service_client.client.h" containing the
/// concrete UserServiceClientImpl that makes the actual HTTP calls.
///
/// Usage:
///   #include "client/user_service_client.h"
///   auto handle = novaboot::client::RestClientFactory::make<UserServiceClient>(event_loop);
///   auto resp = handle->get_user(42);
///   std::cout << resp.body().name << "\n";

#include "model/user.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/router/web_attributes.h"

#include <vector>

class [[=novaboot::web::rest_client{"https://localhost:4433","http2"}]] UserServiceClient {
public:
    virtual ~UserServiceClient() = default;

    /// GET /api/users  → list of all users
    [[=novaboot::web::get{"/api/users"}]]
    virtual novaboot::ResponseEntity<std::vector<examples::model::User>> get_all_users() = 0;

    /// GET /api/users/{id}  → single user
    [[=novaboot::web::get{"/api/users/{id}"}]]
    virtual novaboot::ResponseEntity<examples::model::User> get_user(int id) = 0;

    /// POST /api/users  → create user (body = User JSON)
    [[=novaboot::web::post{"/api/users"}]]
    virtual novaboot::ResponseEntity<examples::model::User> create_user(
        examples::model::User user) = 0;

    /// PUT /api/users/{id}  → replace user
    [[=novaboot::web::put{"/api/users/{id}"}]]
    virtual novaboot::ResponseEntity<examples::model::User> update_user(
        int id, examples::model::User user) = 0;

    /// DELETE /api/users/{id}  → remove user
    [[=novaboot::web::del{"/api/users/{id}"}]]
    virtual novaboot::ResponseEntity<void> delete_user(int id) = 0;
};
