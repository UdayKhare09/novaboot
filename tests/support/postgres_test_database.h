#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "novaboot/db/drivers/postgres/postgres_driver.h"

namespace novaboot::testing {

/// A per-test PostgreSQL schema with deterministic RAII cleanup.
///
/// Set NOVABOOT_POSTGRES_TEST_URL to a libpq connection string or URI to use a
/// disposable CI database. The local Docker-development default is retained
/// only when that variable is not supplied.
class PostgresTestDatabase {
public:
    PostgresTestDatabase() {
        const char* configured = std::getenv("NOVABOOT_POSTGRES_TEST_URL");
        connection_info_ = configured != nullptr && *configured != '\0'
            ? configured
            : "host=localhost dbname=postgres user=postgres password=postgres connect_timeout=2";
        admin_ = std::make_shared<db::postgres::PostgresDataSource>(connection_info_, 1);
        schema_ = "novaboot_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(++next_schema_id_);
        auto connection = admin_->get_connection();
        connection->execute("CREATE SCHEMA " + schema_);
        connection.reset();
        datasource_ = std::make_shared<db::postgres::PostgresDataSource>(
            connection_info_, 3, std::chrono::seconds{2}, std::chrono::seconds{30},
            "SET search_path TO " + schema_);
    }

    ~PostgresTestDatabase() {
        datasource_.reset();
        try {
            if (admin_) {
                auto connection = admin_->get_connection();
                connection->execute("DROP SCHEMA IF EXISTS " + schema_ + " CASCADE");
            }
        } catch (...) {
            // Test cleanup must not mask the actual assertion failure.
        }
    }

    PostgresTestDatabase(const PostgresTestDatabase&) = delete;
    PostgresTestDatabase& operator=(const PostgresTestDatabase&) = delete;

    [[nodiscard]] std::shared_ptr<db::postgres::PostgresDataSource> datasource() const {
        return datasource_;
    }

private:
    inline static std::atomic_uint64_t next_schema_id_{0};
    std::string connection_info_;
    std::string schema_;
    std::shared_ptr<db::postgres::PostgresDataSource> admin_;
    std::shared_ptr<db::postgres::PostgresDataSource> datasource_;
};

} // namespace novaboot::testing
