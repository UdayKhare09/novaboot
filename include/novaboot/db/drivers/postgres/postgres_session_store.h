#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "novaboot/db/db_client.h"
#include "novaboot/middleware/session_middleware.h"

namespace novaboot::db::postgres {

/// Durable shared SessionStore backed by an application-managed PostgreSQL
/// table. Run the documented DDL through NovaBoot migrations before creating
/// the store; this class never changes an application's schema implicitly.
class PostgresSessionStore final : public middleware::SessionStore {
public:
    /// `table_name` is an SQL identifier, optionally schema-qualified.
    /// The default table is `novaboot_sessions`.
    explicit PostgresSessionStore(std::shared_ptr<DataSource> datasource,
                                  std::string table_name = "novaboot_sessions");

    void put(middleware::Session session) override;
    [[nodiscard]] std::optional<middleware::Session>
    find(std::string_view id, std::chrono::system_clock::time_point now) override;
    void erase(std::string_view id) override;

    /// Delete expired records. Call from scheduled maintenance when desired;
    /// `find` also deletes the specific expired session it encounters.
    std::int64_t erase_expired(std::chrono::system_clock::time_point now);

    /// PostgreSQL migration DDL for the default table shape.
    [[nodiscard]] static std::string schema_ddl(std::string_view table_name = "novaboot_sessions");

private:
    [[nodiscard]] static bool valid_table_name(std::string_view table_name) noexcept;
    [[nodiscard]] std::string insert_sql() const;
    [[nodiscard]] std::string select_sql() const;
    [[nodiscard]] std::string delete_sql() const;
    [[nodiscard]] std::string delete_expired_sql() const;

    std::shared_ptr<DataSource> datasource_;
    std::string table_name_;
};

} // namespace novaboot::db::postgres
