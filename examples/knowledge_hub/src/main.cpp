#include "config/web_config.h"
#include "controller/global_exception_handler.h"
#include "controller/knowledge_controller.h"
#include "model/entities.h"
#include "novaboot/config/app_config.h"
#include "novaboot/db/migration.h"
#include "novaboot/db/schema.h"
#include "novaboot/novaboot.h"
#include "repository/article_repository.h"
#include "repository/contributor_repository.h"
#include "repository/project_repository.h"
#include "service/knowledge_service.h"

#include <cstdlib>
#include <memory>
#include <spdlog/spdlog.h>
#include <thread>

using namespace knowledge_hub::config;
using namespace knowledge_hub::controller;
using namespace knowledge_hub::model;
using namespace knowledge_hub::repository;
using namespace knowledge_hub::service;
using namespace novaboot;
using namespace novaboot::config;
using namespace novaboot::di;

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Starting Knowledge Hub example...");

    auto cfg = AppConfig::load("examples/knowledge_hub/src/resources/config.toml");

    RootContainer di_root;
    di_root.register_bean<AppConfig>([cfg](ContainerBase&) {
        return new AppConfig(cfg);
    });

    novaboot::annotations::register_beans<
        ProjectRepository, ContributorRepository, ArticleRepository,
        KnowledgeService,
        KnowledgeController, GlobalExceptionHandler,
        WebConfig
    >(di_root);

    di_root.build();

    {
        auto datasource = di_root.resolve<std::shared_ptr<novaboot::db::DataSource>>();
        try {
            novaboot::db::MigrationRunner::run(*datasource, {
                novaboot::db::Migration::sql(
                    1,
                    "create knowledge hub audit table",
                    "CREATE TABLE IF NOT EXISTS kh_audit_events ("
                    "id BIGSERIAL PRIMARY KEY, "
                    "message TEXT NOT NULL, "
                    "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
                ),
            });

            novaboot::db::SchemaGenerator::create_table<Contributor>(*datasource);
            novaboot::db::SchemaGenerator::create_table<Project>(*datasource);
            novaboot::db::SchemaGenerator::create_table<Article>(*datasource);
        } catch (const std::exception& error) {
            spdlog::critical("Knowledge Hub bootstrap failed: {}", error.what());
            di_root.shutdown();
            return EXIT_FAILURE;
        }
    }

    auto worker_count = static_cast<int>(cfg.server().workers);
    if (worker_count == 0) {
        worker_count = static_cast<int>(std::thread::hardware_concurrency());
    }

    auto app = Server::create()
        .workers(worker_count)
        .bind(cfg.server().host, cfg.server().port)
        .tls(cfg.server().tls_cert, cfg.server().tls_key)
        .di_container(di_root)
        .static_resources(cfg.server().static_resources)
        .build();

    app->run();

    di_root.shutdown();
    return 0;
}
