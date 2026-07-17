#pragma once

#include "novaboot/db/db_client.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace novaboot::db {

class MigrationException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One intentional, application-supplied database change. NovaBoot records the
/// version after the callback succeeds; it never derives migrations from an
/// entity diff.
struct Migration {
    std::int64_t version = 0;
    std::string description;
    std::function<void(Connection&)> apply;

    static Migration sql(std::int64_t version, std::string description,
                         std::string statement) {
        return Migration{
            .version = version,
            .description = std::move(description),
            .apply = [statement = std::move(statement)](Connection& connection) {
                connection.execute(statement);
            },
        };
    }
};

class MigrationRunner {
private:
    static void ensure_ledger(Connection& connection) {
        connection.execute(
            "CREATE TABLE IF NOT EXISTS novaboot_schema_migrations ("
            "version BIGINT PRIMARY KEY, "
            "description VARCHAR(255) NOT NULL"
            ")");
    }

public:
    static void run(DataSource& datasource, std::vector<Migration> migrations) {
        std::sort(migrations.begin(), migrations.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.version < rhs.version;
        });

        std::unordered_set<std::int64_t> declared_versions;
        for (const auto& migration : migrations) {
            if (migration.version <= 0 || migration.description.empty() || !migration.apply) {
                throw MigrationException("Each migration needs a positive version, description, and callback");
            }
            if (!declared_versions.insert(migration.version).second) {
                throw MigrationException("Migration versions must be unique");
            }
        }

        auto connection = datasource.get_connection();
        ensure_ledger(*connection);

        std::unordered_set<std::int64_t> applied_versions;
        {
            auto result = connection->query("SELECT version FROM novaboot_schema_migrations");
            while (result->next()) applied_versions.insert(result->get_int(0));
        }
        for (const auto version : applied_versions) {
            if (!declared_versions.contains(version)) {
                throw MigrationException("Database contains migration version " + std::to_string(version) +
                                         " that is not declared by this application");
            }
        }

        for (const auto& migration : migrations) {
            if (applied_versions.contains(migration.version)) continue;

            connection->begin_transaction();
            try {
                migration.apply(*connection);
                connection->execute(
                    "INSERT INTO novaboot_schema_migrations (version, description) VALUES (?, ?)",
                    {Parameter(migration.version), Parameter(migration.description)});
                connection->commit();
            } catch (...) {
                try {
                    connection->rollback();
                } catch (...) {
                    // Preserve the migration failure as the primary diagnostic.
                }
                throw;
            }
        }
    }
};

} // namespace novaboot::db
