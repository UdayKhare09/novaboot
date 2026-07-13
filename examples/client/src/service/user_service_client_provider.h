#pragma once
#include "novaboot/core/io_uring_event_loop.h"
#include "client/user_service_client.h"

#include <memory>

class UserServiceClientProvider {
public:
    std::unique_ptr<novaboot::core::IoUringEventLoop> event_loop;
    std::unique_ptr<UserServiceClient> client;

    UserServiceClientProvider() {
        event_loop = std::make_unique<novaboot::core::IoUringEventLoop>();
        client = std::make_unique<UserServiceClient>(*event_loop);
    }
};
