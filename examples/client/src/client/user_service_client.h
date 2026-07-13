#pragma once

#include "model/user.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/client/rest_client.h"
#include "novaboot/router/json.h"

#include <vector>
#include <memory>

class UserServiceClient {
private:
    std::unique_ptr<novaboot::client::RestClient> client_;

public:
    explicit UserServiceClient(novaboot::core::EventLoop& event_loop) {
        client_ = novaboot::client::RestClient::builder()
            .host("localhost")
            .port(4433)
            .verify_ssl(false) // For testing localhost cert
            .protocol(novaboot::client::Protocol::HTTP2)
            .build(event_loop);
    }

    novaboot::ResponseEntity<std::vector<examples::model::User>> get_all_users() {
        auto resp = client_->get("/api/users");
        auto users = novaboot::json::deserialize<std::vector<examples::model::User>>(resp.body);
        return novaboot::ResponseEntity<std::vector<examples::model::User>>::status(resp.status_code, users);
    }

    novaboot::ResponseEntity<examples::model::User> get_user(int id) {
        auto resp = client_->get("/api/users/" + std::to_string(id));
        auto user = novaboot::json::deserialize<examples::model::User>(resp.body);
        return novaboot::ResponseEntity<examples::model::User>::status(resp.status_code, user);
    }

    novaboot::ResponseEntity<examples::model::User> create_user(examples::model::User user) {
        auto body = novaboot::json::serialize(user);
        auto resp = client_->post("/api/users", body);
        auto saved = novaboot::json::deserialize<examples::model::User>(resp.body);
        return novaboot::ResponseEntity<examples::model::User>::status(resp.status_code, saved);
    }

    novaboot::ResponseEntity<examples::model::User> update_user(int id, examples::model::User user) {
        auto body = novaboot::json::serialize(user);
        auto resp = client_->put("/api/users/" + std::to_string(id), body);
        auto saved = novaboot::json::deserialize<examples::model::User>(resp.body);
        return novaboot::ResponseEntity<examples::model::User>::status(resp.status_code, saved);
    }

    novaboot::ResponseEntity<void> delete_user(int id) {
        auto resp = client_->del("/api/users/" + std::to_string(id));
        return novaboot::ResponseEntity<void>::status(resp.status_code);
    }
};
