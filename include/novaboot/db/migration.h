#pragma once

#include "novaboot/db/db_client.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
    std::string checksum;
    std::function<void(Connection&)> apply;

    static std::string checksum_of(std::string_view text) {
        // Stable FNV-1a 64-bit checksum. This is intended to detect accidental
        // migration edits, not to be a cryptographic signature.
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto ch : text) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ull;
        }
        std::ostringstream out;
        out << std::hex << std::setw(16) << std::setfill('0') << hash;
        return out.str();
    }

    static Migration sql(std::int64_t version, std::string description,
                         std::string statement) {
        const auto checksum = checksum_of(statement);
        return Migration{
            .version = version,
            .description = std::move(description),
            .checksum = checksum,
            .apply = [statement = std::move(statement)](Connection& connection) {
                connection.execute(statement);
            },
        };
    }

    static Migration callback(std::int64_t version, std::string description,
                              std::string checksum,
                              std::function<void(Connection&)> apply) {
        return Migration{
            .version = version,
            .description = std::move(description),
            .checksum = std::move(checksum),
            .apply = std::move(apply),
        };
    }
};

struct AppliedMigration {
    std::int64_t version = 0;
    std::string description;
    std::string checksum;
    bool success = false;
};

class MigrationRunner {
private:
    static void ensure_ledger(Connection& connection) {
        connection.execute(
            "CREATE TABLE IF NOT EXISTS novaboot_schema_migrations ("
            "version BIGINT PRIMARY KEY, "
            "description VARCHAR(255) NOT NULL, "
            "checksum VARCHAR(64) NOT NULL, "
            "success BOOLEAN NOT NULL, "
            "installed_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ")");

        // Best-effort upgrade for ledgers created by early NovaBoot builds.
        // Ignore "duplicate column" errors so this stays portable and idempotent.
        try {
            connection.execute(
                "ALTER TABLE novaboot_schema_migrations "
                "ADD COLUMN checksum VARCHAR(64) NOT NULL DEFAULT 'legacy'");
        } catch (...) {}
        try {
            connection.execute(
                "ALTER TABLE novaboot_schema_migrations "
                "ADD COLUMN success BOOLEAN NOT NULL DEFAULT true");
        } catch (...) {}
        try {
            connection.execute(
                "ALTER TABLE novaboot_schema_migrations "
                "ADD COLUMN installed_at TIMESTAMP");
        } catch (...) {}
    }

    static void normalize(std::vector<Migration>& migrations) {
        std::sort(migrations.begin(), migrations.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.version < rhs.version;
        });

        std::unordered_set<std::int64_t> declared_versions;
        for (auto& migration : migrations) {
            if (migration.version <= 0 || migration.description.empty() || !migration.apply) {
                throw MigrationException("Each migration needs a positive version, description, and callback");
            }
            if (!declared_versions.insert(migration.version).second) {
                throw MigrationException("Migration versions must be unique");
            }
            if (migration.checksum.empty()) {
                migration.checksum = Migration::checksum_of(migration.description);
            }
        }
    }

    static std::unordered_map<std::int64_t, AppliedMigration>
    applied_map(Connection& connection) {
        std::unordered_map<std::int64_t, AppliedMigration> applied;
        auto result = connection.query(
            "SELECT version, description, checksum, success "
            "FROM novaboot_schema_migrations");
        while (result->next()) {
            auto row = AppliedMigration{
                .version = result->get_int(0),
                .description = result->get_string(1),
                .checksum = result->get_string(2),
                .success = result->get_bool(3),
            };
            applied.emplace(row.version, std::move(row));
        }
        return applied;
    }

    static void record_failed(Connection& connection, const Migration& migration) {
        try {
            connection.execute("DELETE FROM novaboot_schema_migrations WHERE version = ?",
                               {Parameter(migration.version)});
            connection.execute(
                "INSERT INTO novaboot_schema_migrations "
                "(version, description, checksum, success) VALUES (?, ?, ?, ?)",
                {
                    Parameter(migration.version),
                    Parameter(migration.description),
                    Parameter(migration.checksum),
                    Parameter(false),
                });
        } catch (...) {
            // Preserve the original migration failure as the primary diagnostic.
        }
    }

public:
    static std::vector<AppliedMigration> applied(DataSource& datasource) {
        auto connection = datasource.get_connection();
        ensure_ledger(*connection);
        auto rows = applied_map(*connection);
        std::vector<AppliedMigration> result;
        result.reserve(rows.size());
        for (auto& [_, row] : rows) result.push_back(std::move(row));
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.version < rhs.version;
        });
        return result;
    }

    static void run(DataSource& datasource, std::vector<Migration> migrations) {
        normalize(migrations);

        auto connection = datasource.get_connection();
        ensure_ledger(*connection);

        const auto applied = applied_map(*connection);
        std::unordered_map<std::int64_t, const Migration*> declared;
        for (const auto& migration : migrations) {
            declared.emplace(migration.version, &migration);
        }

        for (const auto& [version, row] : applied) {
            if (!row.success) {
                throw MigrationException("Database contains failed migration version " +
                                         std::to_string(version) +
                                         "; fix the database or repair the migration ledger");
            }

            const auto declared_it = declared.find(version);
            if (declared_it == declared.end()) {
                throw MigrationException("Database contains migration version " + std::to_string(version) +
                                         " that is not declared by this application");
            }

            const auto& migration = *declared_it->second;
            if (row.description != migration.description) {
                throw MigrationException("Migration version " + std::to_string(version) +
                                         " description changed after it was applied");
            }
            if (row.checksum != "legacy" && row.checksum != migration.checksum) {
                throw MigrationException("Migration version " + std::to_string(version) +
                                         " checksum changed after it was applied");
            }
        }

        for (const auto& migration : migrations) {
            if (applied.contains(migration.version)) continue;

            connection->begin_transaction();
            try {
                connection->execute(
                    "INSERT INTO novaboot_schema_migrations "
                    "(version, description, checksum, success) VALUES (?, ?, ?, ?)",
                    {
                        Parameter(migration.version),
                        Parameter(migration.description),
                        Parameter(migration.checksum),
                        Parameter(false),
                    });
                migration.apply(*connection);
                connection->execute(
                    "UPDATE novaboot_schema_migrations SET success = ? WHERE version = ?",
                    {Parameter(true), Parameter(migration.version)});
                connection->commit();
            } catch (...) {
                try {
                    connection->rollback();
                } catch (...) {
                    // Preserve the migration failure as the primary diagnostic.
                }
                record_failed(*connection, migration);
                throw;
            }
        }
    }
};

} // namespace novaboot::db
