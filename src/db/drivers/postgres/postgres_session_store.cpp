#include "novaboot/db/drivers/postgres/postgres_session_store.h"

#include <cctype>
#include <stdexcept>
#include <utility>
#include <vector>

#include "novaboot/router/json.h"

namespace novaboot::db::postgres {

PostgresSessionStore::PostgresSessionStore(std::shared_ptr<DataSource> datasource,
                                           std::string table_name)
    : datasource_(std::move(datasource)), table_name_(std::move(table_name)) {
    if (!datasource_) {
        throw std::invalid_argument("PostgresSessionStore requires a DataSource");
    }
    if (!valid_table_name(table_name_)) {
        throw std::invalid_argument("PostgresSessionStore table_name must be a simple SQL identifier");
    }
}

bool PostgresSessionStore::valid_table_name(std::string_view table_name) noexcept {
    if (table_name.empty()) return false;
    bool segment_start = true;
    for (const unsigned char character : table_name) {
        if (character == '.') {
            if (segment_start) return false;
            segment_start = true;
            continue;
        }
        if (!(std::isalnum(character) || character == '_')) return false;
        if (segment_start && std::isdigit(character)) return false;
        segment_start = false;
    }
    return !segment_start;
}

std::string PostgresSessionStore::insert_sql() const {
    return "INSERT INTO " + table_name_ +
        " (id, subject, roles_json, scopes_json, expires_at) VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT (id) DO UPDATE SET subject = EXCLUDED.subject, "
        "roles_json = EXCLUDED.roles_json, scopes_json = EXCLUDED.scopes_json, "
        "expires_at = EXCLUDED.expires_at";
}

std::string PostgresSessionStore::select_sql() const {
    return "SELECT subject, roles_json, scopes_json, expires_at FROM " + table_name_ +
        " WHERE id = ? AND expires_at > ?";
}

std::string PostgresSessionStore::delete_sql() const {
    return "DELETE FROM " + table_name_ + " WHERE id = ?";
}

std::string PostgresSessionStore::delete_expired_sql() const {
    return "DELETE FROM " + table_name_ + " WHERE expires_at <= ?";
}

void PostgresSessionStore::put(middleware::Session session) {
    if (session.id.empty() || session.principal.subject.empty()) {
        throw std::invalid_argument("PostgresSessionStore requires a session id and subject");
    }
    auto connection = datasource_->get_connection();
    connection->execute(insert_sql(), {
        std::move(session.id),
        std::move(session.principal.subject),
        json::serialize(session.principal.roles),
        json::serialize(session.principal.scopes),
        session.expires_at,
    });
}

std::optional<middleware::Session>
PostgresSessionStore::find(std::string_view id, std::chrono::system_clock::time_point now) {
    if (id.empty()) return std::nullopt;
    auto connection = datasource_->get_connection();
    auto result = connection->query(select_sql(), {std::string(id), now});
    if (!result->next()) return std::nullopt;

    try {
        middleware::Session session{
            .id = std::string(id),
            .principal = {
                .subject = result->get_string(0),
                .roles = json::deserialize<std::vector<std::string>>(result->get_string(1)),
                .scopes = json::deserialize<std::vector<std::string>>(result->get_string(2)),
            },
            .expires_at = result->get_time(3),
        };
        if (session.principal.subject.empty() || session.expires_at <= now) {
            connection->execute(delete_sql(), {std::string(id)});
            return std::nullopt;
        }
        return session;
    } catch (const json::BindingException&) {
        connection->execute(delete_sql(), {std::string(id)});
        return std::nullopt;
    }
}

void PostgresSessionStore::erase(std::string_view id) {
    if (id.empty()) return;
    auto connection = datasource_->get_connection();
    connection->execute(delete_sql(), {std::string(id)});
}

std::int64_t PostgresSessionStore::erase_expired(std::chrono::system_clock::time_point now) {
    auto connection = datasource_->get_connection();
    return connection->execute(delete_expired_sql(), {now});
}

std::string PostgresSessionStore::schema_ddl(std::string_view table_name) {
    if (!valid_table_name(table_name)) {
        throw std::invalid_argument("PostgresSessionStore table_name must be a simple SQL identifier");
    }
    return "CREATE TABLE " + std::string(table_name) + " ("
           "id VARCHAR(128) PRIMARY KEY, "
           "subject TEXT NOT NULL, "
           "roles_json TEXT NOT NULL, "
           "scopes_json TEXT NOT NULL, "
           "expires_at TIMESTAMPTZ NOT NULL"
           ")";
}

} // namespace novaboot::db::postgres
