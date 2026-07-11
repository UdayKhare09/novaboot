#pragma once
#include "novaboot/di/di.h"
#include "novaboot/core/io_uring_event_loop.h"
#include "novaboot/client/rest_client_factory.h"
#include "client/user_service_client.client.h"

#include <memory>

class [[=novaboot::di::component{}]] UserServiceClientProvider {
public:
    std::unique_ptr<novaboot::core::IoUringEventLoop> event_loop;
    std::unique_ptr<UserServiceClient> client;

    UserServiceClientProvider() {
        event_loop = std::make_unique<novaboot::core::IoUringEventLoop>();
        client = novaboot::client::RestClientFactory::make<
            UserServiceClient, UserServiceClientImpl>(*event_loop);
    }
};
