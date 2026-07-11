#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/config/app_config.h"

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

    // Register all scanned components (UserServiceClientProvider, GatewayUserController)
    novaboot_di_register_all(di_root);

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
    novaboot_web_register_all(*app);

    // 3. Run
    app->run();

    // Shutdown DI container
    di_root.shutdown();
    return 0;
}
