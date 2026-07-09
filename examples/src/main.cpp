#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/middleware/middleware.h"

#include "repository/user_repository.h"
#include "service/user_service.h"
#include "service/request_logger.h"
#include "controller/user_controller.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <memory>

using namespace novaboot;
using namespace novaboot::di;

// Forward declare the auto-generated CMake registrations function
void novaboot_di_register_all(RootContainer& root);

int main() {
    // Set logging level
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting Spring Boot-style C++ DI Sample App");

    // 1. DI Container Setup
    RootContainer di_root;

    // Register all scanned components (UserRepository, UserService, RequestLogger, UserController)
    novaboot_di_register_all(di_root);

    // Build the container: builds dependency graph and instantiates singletons
    di_root.build();

    // 2. Server Setup (pass di_root for automatic request-scoped DI resolution)
    auto app = Server::create()
        .workers(std::thread::hardware_concurrency())
        .bind("0.0.0.0", 4433)
        .tls("cert.pem", "key.pem")
        .di_container(di_root)
        .build();

    // 3. Web Routing Mapping (Auto-registers all UserController methods with web attributes)
    app->register_controllers<UserController>();

    // 4. Run the Server
    app->run();

    // Shutdown DI Container to invoke pre-destroy hooks
    di_root.shutdown();
    
    return 0;
}
