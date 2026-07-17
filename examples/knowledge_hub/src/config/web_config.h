#pragma once

#include "novaboot/config/app_config.h"
#include "novaboot/db/drivers/postgres/postgres_driver.h"
#include "novaboot/db/transaction.h"
#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"
#include "novaboot/middleware/security_headers_middleware.h"
#include "novaboot/novaboot.h"

#include <memory>
#include <string>

namespace knowledge_hub::config {

using namespace novaboot::annotations;
using namespace novaboot::di;
using namespace novaboot::middleware;

struct [[= Configuration() ]] WebConfig {
    [[= Value("database.connection") ]]
    std::string database_connection = "host=localhost dbname=postgres user=postgres password=postgres connect_timeout=5";

    [[= Value("database.show-sql") ]]
    bool show_sql = false;

    [[= Bean() ]]
    std::shared_ptr<novaboot::db::DataSource> datasource() {
        novaboot::db::show_sql = show_sql;
        return std::make_shared<novaboot::db::postgres::PostgresDataSource>(
            database_connection, 4);
    }

    [[= Bean() ]]
    novaboot::db::TransactionManager* transaction_manager(
        std::shared_ptr<novaboot::db::DataSource> datasource) {
        return new novaboot::db::TransactionManager(std::move(datasource));
    }

    [[= Bean() ]] [[= Order(1) ]]
    std::shared_ptr<CorsMiddleware> cors_middleware() {
        return std::make_shared<CorsMiddleware>(
            CorsMiddleware::Config{
                .allowed_origins = {"*"},
                .allowed_methods = {"GET", "POST", "PUT", "DELETE", "OPTIONS"},
                .allowed_headers = {"Content-Type", "Authorization"},
                .allow_credentials = false,
                .max_age_seconds = 86400,
            });
    }

    [[= Bean() ]] [[= Order(2) ]]
    std::shared_ptr<SecurityHeadersMiddleware> security_headers() {
        return std::make_shared<SecurityHeadersMiddleware>();
    }

    [[= Bean() ]] [[= Order(3) ]]
    std::shared_ptr<RequestLoggingMiddleware> request_logging() {
        return std::make_shared<RequestLoggingMiddleware>();
    }
};

} // namespace knowledge_hub::config
