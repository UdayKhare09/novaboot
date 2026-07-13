#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/config/app_config.h"

#include "service/user_service_client_provider.h"
#include "controller/gateway_user_controller.h"

#include <spdlog/spdlog.h>
#include <memory>

using namespace novaboot;
using namespace novaboot::di;

int main() {
    // Set logging level
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting Gateway Proxy Server on port 4434...");

    // 0. Load TOML configuration
    auto cfg = novaboot::config::AppConfig::load("examples/client/src/resources/config.toml");

    // 1. DI Container Setup
    RootContainer di_root;

    // Register AppConfig
    di_root.register_bean<novaboot::config::AppConfig>([cfg](ContainerBase&) {
        return new novaboot::config::AppConfig(cfg);
    });

    // Manually register components
    di_root.singleton<UserServiceClientProvider>([](auto&) {
        return new UserServiceClientProvider();
    });
    di_root.singleton<GatewayUserController>([](auto& c) {
        return new GatewayUserController(c.template resolve<UserServiceClientProvider>());
    }).depends_on<UserServiceClientProvider>();

    // Build container
    di_root.build();

    // 2. Server Setup
    auto app = Server::create()
        .workers(static_cast<int>(cfg.server().workers))
        .bind(cfg.server().host, cfg.server().port)
        .tls(cfg.server().tls_cert, cfg.server().tls_key)
        .di_container(di_root)
        .build();

    // Register gateway routes
    app->router().group("/gateway/users")
        .get("", di::handler<&GatewayUserController::get_all>())
        .get("/:id", di::handler<&GatewayUserController::get_one>())
        .post("", di::handler<&GatewayUserController::create>())
        .put("/:id", di::handler<&GatewayUserController::update>())
        .del("/:id", di::handler<&GatewayUserController::remove>());

    // 3. Run
    app->run();

    // Shutdown DI container
    di_root.shutdown();
    return 0;
}
