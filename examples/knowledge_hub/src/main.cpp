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
#include <vector>

using namespace knowledge_hub::config;
using namespace knowledge_hub::controller;
using namespace knowledge_hub::model;
using namespace knowledge_hub::repository;
using namespace knowledge_hub::service;
using namespace novaboot;
using namespace novaboot::config;
using namespace novaboot::di;

static std::vector<novaboot::db::Migration> knowledge_hub_migrations() {
    return {
        novaboot::db::Migration::sql(
            1,
            "create knowledge hub audit table",
            "CREATE TABLE IF NOT EXISTS kh_audit_events ("
            "id BIGSERIAL PRIMARY KEY, "
            "message TEXT NOT NULL, "
            "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
        ),
        novaboot::db::Migration::sql(
            2,
            "create knowledge hub projects",
            "CREATE TABLE IF NOT EXISTS kh_projects ("
            "id BIGSERIAL PRIMARY KEY, "
            "slug VARCHAR(80) NOT NULL UNIQUE, "
            "name VARCHAR(160) NOT NULL, "
            "description VARCHAR(255), "
            "settings JSONB)"
        ),
        novaboot::db::Migration::sql(
            3,
            "create knowledge hub contributors",
            "CREATE TABLE IF NOT EXISTS kh_contributors ("
            "id BIGSERIAL PRIMARY KEY, "
            "handle VARCHAR(80) NOT NULL UNIQUE, "
            "display_name VARCHAR(255), "
            "role VARCHAR(255))"
        ),
        novaboot::db::Migration::sql(
            4,
            "create knowledge hub articles",
            "CREATE TABLE IF NOT EXISTS kh_articles ("
            "id BIGSERIAL PRIMARY KEY, "
            "title VARCHAR(180) NOT NULL, "
            "body VARCHAR(255), "
            "status VARCHAR(255), "
            "published_at TIMESTAMP, "
            "metadata JSONB, "
            "project_id BIGINT REFERENCES kh_projects(id))"
        ),
        novaboot::db::Migration::sql(
            5,
            "create knowledge hub article contributors join table",
            "CREATE TABLE IF NOT EXISTS kh_article_contributors ("
            "article_id BIGINT NOT NULL REFERENCES kh_articles(id), "
            "contributor_id BIGINT NOT NULL REFERENCES kh_contributors(id), "
            "PRIMARY KEY (article_id, contributor_id))"
        ),
    };
}

static void validate_knowledge_hub_schema(novaboot::db::DataSource& datasource) {
    novaboot::db::SchemaGenerator::validate_table<Contributor>(datasource);
    novaboot::db::SchemaGenerator::validate_table<Project>(datasource);
    novaboot::db::SchemaGenerator::validate_table<Article>(datasource);
}

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
            novaboot::db::MigrationRunner::run(*datasource, knowledge_hub_migrations());
            validate_knowledge_hub_schema(*datasource);
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
