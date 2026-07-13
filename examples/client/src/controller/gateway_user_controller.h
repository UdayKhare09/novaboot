#pragma once
#include "novaboot/router/response_entity.h"
#include "novaboot/context/request_context.h"
#include "service/user_service_client_provider.h"
#include "model/user.h"

#include <vector>

struct GatewayUserController {
    UserServiceClientProvider& provider;

    explicit GatewayUserController(UserServiceClientProvider& p) : provider(p) {}

    novaboot::ResponseEntity<std::vector<examples::model::User>> get_all(novaboot::context::RequestContext&) {
        return provider.client->get_all_users();
    }

    novaboot::ResponseEntity<examples::model::User> get_one(int id, novaboot::context::RequestContext&) {
        return provider.client->get_user(id);
    }

    novaboot::ResponseEntity<examples::model::User> create(
        examples::model::User user,
        novaboot::context::RequestContext&
    ) {
        return provider.client->create_user(user);
    }

    novaboot::ResponseEntity<examples::model::User> update(
        int id,
        examples::model::User user,
        novaboot::context::RequestContext&
    ) {
        return provider.client->update_user(id, user);
    }

    novaboot::ResponseEntity<void> remove(int id, novaboot::context::RequestContext&) {
        return provider.client->delete_user(id);
    }
};
