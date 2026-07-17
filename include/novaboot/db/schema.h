#pragma once

#include "novaboot/annotations/stereotypes.h"
#include "novaboot/db/db_client.h"
#include "novaboot/db/orm_reflection.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace novaboot::db {

class SchemaMismatchException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Reflection-driven, deliberately small DDL generator. It creates missing
/// tables but never alters existing ones; migrations remain an explicit step.
class SchemaGenerator {
private:
    struct ColumnSpec {
        std::string name;
        std::string type;
        int length = -1;
        bool nullable = true;
        bool primary_key = false;
        bool unique = false;
    };

    static std::string canonical_type(std::string type) {
        std::string result;
        result.reserve(type.size());
        for (const auto character : type) {
            if (!std::isspace(static_cast<unsigned char>(character))) {
                result += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            }
        }
        const auto paren = result.find('(');
        if (paren != std::string::npos) result.resize(paren);
        if (result == "CHARACTERVARYING") return "VARCHAR";
        if (result == "TIMESTAMPWITHOUTTIMEZONE") return "TIMESTAMP";
        if (result == "INT" || result == "INT4") return "INTEGER";
        if (result == "INT8") return "BIGINT";
        return result;
    }

    static int type_length(std::string_view type) {
        const auto open = type.find('(');
        const auto close = type.find(')', open);
        if (open == std::string_view::npos || close == std::string_view::npos) return -1;
        try {
            return std::stoi(std::string(type.substr(open + 1, close - open - 1)));
        } catch (...) {
            return -1;
        }
    }
    template<typename Entity>
    static std::string table_name() {
        using namespace novaboot::annotations;
        if constexpr (novaboot::di::detail::has_annotation<Table>(^^Entity)) {
            constexpr auto table = novaboot::di::detail::get_annotation<Table>(^^Entity);
            if constexpr (table.name[0] != '\0') {
                if constexpr (table.schema[0] != '\0') {
                    return std::string(table.schema) + "." + table.name;
                }
                return table.name;
            }
        }

        constexpr auto entity = detail::get_table_metadata<Entity>();
        if constexpr (entity.name[0] != '\0') return entity.name;

        constexpr auto raw_name = std::meta::identifier_of(^^Entity);
        return detail::to_snake_case(raw_name) + "s";
    }

    template<typename Field>
    static std::string portable_type(const SqlDialect& dialect, int length, bool lob,
                                     novaboot::annotations::EnumType enum_type) {
        const bool postgres = dialect.name() == "postgresql";
        if constexpr (std::is_same_v<Field, bool>) {
            return "BOOLEAN";
        } else if constexpr (std::is_same_v<Field, int> ||
                             std::is_same_v<Field, std::int32_t>) {
            return "INTEGER";
        } else if constexpr (std::is_same_v<Field, std::int64_t>) {
            return "BIGINT";
        } else if constexpr (std::is_same_v<Field, float>) {
            return postgres ? "REAL" : "REAL";
        } else if constexpr (std::is_same_v<Field, double>) {
            return postgres ? "DOUBLE PRECISION" : "REAL";
        } else if constexpr (std::is_same_v<Field, std::string>) {
            return lob ? "TEXT" : "VARCHAR(" + std::to_string(length) + ")";
        } else if constexpr (std::is_same_v<Field, std::vector<std::uint8_t>>) {
            return postgres ? "BYTEA" : "BLOB";
        } else if constexpr (std::is_same_v<Field, Uuid>) {
            return postgres ? "UUID" : "TEXT";
        } else if constexpr (std::is_same_v<Field, std::chrono::system_clock::time_point>) {
            return postgres ? "TIMESTAMP" : "TEXT";
        } else if constexpr (std::is_enum_v<Field>) {
            return enum_type == novaboot::annotations::EnumType::String
                ? "VARCHAR(" + std::to_string(length) + ")"
                : "INTEGER";
        } else {
            throw std::invalid_argument("SchemaGenerator does not support this entity field type");
        }
    }

    static std::string temporal_type(const SqlDialect& dialect,
                                     novaboot::annotations::TemporalType temporal_type) {
        if (dialect.name() != "postgresql") return "TEXT";
        if (temporal_type == novaboot::annotations::TemporalType::Date) return "DATE";
        if (temporal_type == novaboot::annotations::TemporalType::Time) return "TIME";
        return "TIMESTAMP";
    }

    template<typename Entity>
    static std::string primary_key_column() {
        static constexpr auto members = detail::get_members<Entity>();
        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::Id>(member)) {
                return std::string(detail::get_member_column_name<member>().name);
            }
        }
        throw std::invalid_argument("@ManyToOne target has no @Id field");
    }

    template<typename Entity>
    static std::string primary_key_type(const SqlDialect& dialect) {
        static constexpr auto members = detail::get_members<Entity>();
        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::Id>(member)) {
                using Id = std::remove_cvref_t<decltype(std::declval<Entity&>().[:member:])>;
                return portable_type<Id>(dialect, 255, false,
                                         novaboot::annotations::EnumType::Ordinal);
            }
        }
        throw std::invalid_argument("@ManyToOne target has no @Id field");
    }

    template<typename Entity, std::meta::info Member>
    static std::string column_sql(const SqlDialect& dialect) {
        using namespace novaboot::annotations;
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;

        constexpr auto name = detail::get_member_column_name<Member>();
        constexpr bool is_id = novaboot::di::detail::has_annotation<Id>(Member);
        constexpr bool generated = novaboot::di::detail::has_annotation<GeneratedValue>(Member);
        constexpr bool versioned = novaboot::di::detail::has_annotation<Version>(Member);
        constexpr bool lob = novaboot::di::detail::has_annotation<Lob>(Member);
        constexpr bool json = novaboot::di::detail::has_annotation<Json>(Member);
        constexpr bool temporal = novaboot::di::detail::has_annotation<Temporal>(Member);
        constexpr bool many_to_one = novaboot::di::detail::has_annotation<ManyToOne>(Member);
        constexpr bool enumerated = novaboot::di::detail::has_annotation<Enumerated>(Member);
        constexpr auto enum_type = enumerated
            ? novaboot::di::detail::get_annotation<Enumerated>(Member).value
            : EnumType::Ordinal;

        if constexpr (many_to_one) {
            static_assert(novaboot::di::detail::has_annotation<JoinColumn>(Member),
                          "@ManyToOne requires an explicit @JoinColumn");
            using Target = typename detail::relation_value_type<Field>::type;
            constexpr auto join = novaboot::di::detail::get_annotation<JoinColumn>(Member);
            const auto referenced = join.referenced_column[0] != '\0'
                ? std::string(join.referenced_column)
                : primary_key_column<Target>();
            std::string definition = std::string(name.name) + " " + primary_key_type<Target>(dialect);
            if (!join.nullable) definition += " NOT NULL";
            if (join.unique) definition += " UNIQUE";
            definition += " REFERENCES " + table_name<Target>() + "(" + referenced + ")";
            return definition;
        }

        int length = 255;
        bool nullable = true;
        bool unique = false;
        std::string type;
        if constexpr (novaboot::di::detail::has_annotation<Column>(Member)) {
            constexpr auto column = novaboot::di::detail::get_annotation<Column>(Member);
            length = column.length;
            nullable = column.nullable;
            unique = column.unique;
            if constexpr (column.column_definition[0] != '\0') {
                type = column.column_definition;
            }
        }
        if (type.empty()) {
            if constexpr (json) {
                type = dialect.name() == "postgresql" ? "JSONB" : "TEXT";
            } else if constexpr (std::is_same_v<Field, std::chrono::system_clock::time_point> && temporal) {
                constexpr auto temporal_annotation = novaboot::di::detail::get_annotation<Temporal>(Member);
                type = temporal_type(dialect, temporal_annotation.value);
            } else {
                type = portable_type<Field>(dialect, length, lob, enum_type);
            }
        }

        if constexpr (is_id && generated &&
                      novaboot::di::detail::get_annotation<GeneratedValue>(Member).strategy
                          == GenerationType::AutoIncrement) {
            if (dialect.name() == "postgresql") return std::string(name.name) + " BIGSERIAL PRIMARY KEY";
            return std::string(name.name) + " INTEGER PRIMARY KEY AUTOINCREMENT";
        }

        std::string definition = std::string(name.name) + " " + type;
        if constexpr (is_id) {
            definition += " PRIMARY KEY";
        } else {
            if (!nullable || versioned) definition += " NOT NULL";
            if (unique) definition += " UNIQUE";
        }
        return definition;
    }

    template<typename Entity, std::meta::info Member>
    static ColumnSpec column_spec(const SqlDialect& dialect) {
        using namespace novaboot::annotations;
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;

        constexpr auto name = detail::get_member_column_name<Member>();
        constexpr bool is_id = novaboot::di::detail::has_annotation<Id>(Member);
        constexpr bool generated = novaboot::di::detail::has_annotation<GeneratedValue>(Member);
        constexpr bool versioned = novaboot::di::detail::has_annotation<Version>(Member);
        constexpr bool lob = novaboot::di::detail::has_annotation<Lob>(Member);
        constexpr bool json = novaboot::di::detail::has_annotation<Json>(Member);
        constexpr bool temporal = novaboot::di::detail::has_annotation<Temporal>(Member);
        constexpr bool many_to_one = novaboot::di::detail::has_annotation<ManyToOne>(Member);
        constexpr bool enumerated = novaboot::di::detail::has_annotation<Enumerated>(Member);
        constexpr auto enum_type = enumerated
            ? novaboot::di::detail::get_annotation<Enumerated>(Member).value
            : EnumType::Ordinal;

        if constexpr (many_to_one) {
            static_assert(novaboot::di::detail::has_annotation<JoinColumn>(Member),
                          "@ManyToOne requires an explicit @JoinColumn");
            using Target = typename detail::relation_value_type<Field>::type;
            constexpr auto join = novaboot::di::detail::get_annotation<JoinColumn>(Member);
            const auto type = primary_key_type<Target>(dialect);
            return ColumnSpec{
                .name = name.name,
                .type = canonical_type(type),
                .length = canonical_type(type) == "VARCHAR" ? type_length(type) : -1,
                .nullable = join.nullable,
                .primary_key = false,
                .unique = join.unique,
            };
        }

        int length = 255;
        bool nullable = true;
        bool unique = false;
        std::string type;
        if constexpr (novaboot::di::detail::has_annotation<Column>(Member)) {
            constexpr auto column = novaboot::di::detail::get_annotation<Column>(Member);
            length = column.length;
            nullable = column.nullable;
            unique = column.unique;
            if constexpr (column.column_definition[0] != '\0') type = column.column_definition;
        }
        if (type.empty()) {
            if constexpr (json) {
                type = dialect.name() == "postgresql" ? "JSONB" : "TEXT";
            } else if constexpr (std::is_same_v<Field, std::chrono::system_clock::time_point> && temporal) {
                constexpr auto temporal_annotation = novaboot::di::detail::get_annotation<Temporal>(Member);
                type = temporal_type(dialect, temporal_annotation.value);
            } else {
                type = portable_type<Field>(dialect, length, lob, enum_type);
            }
        }

        if constexpr (is_id && generated &&
                      novaboot::di::detail::get_annotation<GeneratedValue>(Member).strategy
                          == GenerationType::AutoIncrement) {
            type = dialect.name() == "postgresql" ? "BIGINT" : "INTEGER";
        }

        return ColumnSpec{
            .name = name.name,
            .type = canonical_type(type),
            .length = canonical_type(type) == "VARCHAR" ? type_length(type) : -1,
            .nullable = !is_id && !versioned && nullable,
            .primary_key = is_id,
            .unique = !is_id && unique,
        };
    }

    template<typename Entity>
    static std::vector<ColumnSpec> expected_columns(const SqlDialect& dialect) {
        static constexpr auto members = detail::get_members<Entity>();
        std::vector<ColumnSpec> columns;
        template for (constexpr auto member : members) {
            if constexpr (detail::is_persisted_entity_member<member>()) {
                columns.push_back(column_spec<Entity, member>(dialect));
            }
        }
        return columns;
    }

    static bool table_exists(Connection& connection, const SqlDialect& dialect,
                             const std::string& table) {
        if (dialect.name() == "postgresql") {
            auto result = connection.query(
                "SELECT 1 FROM information_schema.tables "
                "WHERE table_schema = current_schema() AND table_name = ?", {Parameter(table)});
            return result->next();
        }
        auto result = connection.query(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", {Parameter(table)});
        return result->next();
    }

    static std::vector<ColumnSpec> sqlite_columns(Connection& connection, const std::string& table) {
        std::vector<ColumnSpec> columns;
        auto result = connection.query("PRAGMA table_info(" + table + ")");
        while (result->next()) {
            const bool primary_key = result->get_int(5) != 0;
            columns.push_back(ColumnSpec{
                .name = result->get_string(1),
                .type = canonical_type(result->get_string(2)),
                .length = type_length(result->get_string(2)),
                .nullable = !primary_key && result->get_int(3) == 0,
                .primary_key = primary_key,
                .unique = false,
            });
        }

        auto indexes = connection.query("PRAGMA index_list(" + table + ")");
        while (indexes->next()) {
            if (indexes->get_int(2) == 0) continue;
            const auto index_name = indexes->get_string(1);
            auto index_columns = connection.query("PRAGMA index_info(" + index_name + ")");
            if (!index_columns->next()) continue;
            const auto column_name = index_columns->get_string(2);
            if (index_columns->next()) continue;
            for (auto& column : columns) {
                if (column.name == column_name && !column.primary_key) column.unique = true;
            }
        }
        return columns;
    }

    static std::vector<ColumnSpec> postgres_columns(Connection& connection,
                                                     const std::string& table) {
        constexpr std::string_view query = R"(
            SELECT c.column_name, c.data_type, c.is_nullable,
                   c.character_maximum_length,
                   EXISTS (
                       SELECT 1 FROM information_schema.table_constraints tc
                       JOIN information_schema.key_column_usage kcu
                         ON tc.constraint_name = kcu.constraint_name
                        AND tc.table_schema = kcu.table_schema
                       WHERE tc.table_schema = c.table_schema
                         AND tc.table_name = c.table_name
                         AND tc.constraint_type = 'PRIMARY KEY'
                         AND kcu.column_name = c.column_name
                   ),
                   EXISTS (
                       SELECT 1 FROM information_schema.table_constraints tc
                       JOIN information_schema.key_column_usage kcu
                         ON tc.constraint_name = kcu.constraint_name
                        AND tc.table_schema = kcu.table_schema
                       WHERE tc.table_schema = c.table_schema
                         AND tc.table_name = c.table_name
                         AND tc.constraint_type = 'UNIQUE'
                         AND kcu.column_name = c.column_name
                   )
              FROM information_schema.columns c
             WHERE c.table_schema = current_schema() AND c.table_name = ?
             ORDER BY c.ordinal_position
        )";
        std::vector<ColumnSpec> columns;
        auto result = connection.query(query, {Parameter(table)});
        while (result->next()) {
            const bool primary_key = result->get_bool(4);
            columns.push_back(ColumnSpec{
                .name = result->get_string(0),
                .type = canonical_type(result->get_string(1)),
                .length = result->is_null(3) ? -1 : static_cast<int>(result->get_int(3)),
                .nullable = !primary_key && result->get_string(2) == "YES",
                .primary_key = primary_key,
                .unique = !primary_key && result->get_bool(5),
            });
        }
        return columns;
    }

    static void require_equal(const std::string& table, const ColumnSpec& expected,
                              const ColumnSpec& actual) {
        const auto fail = [&](std::string_view detail) {
            throw SchemaMismatchException("Schema mismatch for table '" + table +
                                          "', column '" + expected.name + "': " + std::string(detail));
        };
        if (expected.type != actual.type) fail("type differs (expected " + expected.type +
                                               ", found " + actual.type + ")");
        if (expected.length != actual.length) fail("length differs");
        if (expected.nullable != actual.nullable) fail("nullability differs");
        if (expected.primary_key != actual.primary_key) fail("primary-key membership differs");
        if (expected.unique != actual.unique) fail("uniqueness differs");
    }

    static void validate_columns(DataSource& datasource, const std::string& table,
                                 const std::vector<ColumnSpec>& expected) {
        auto dialect = datasource.dialect();
        auto conn = datasource.get_connection();
        if (!table_exists(*conn, *dialect, table)) {
            throw SchemaMismatchException("Expected table '" + table + "' does not exist");
        }

        const auto actual = dialect->name() == "postgresql"
            ? postgres_columns(*conn, table)
            : sqlite_columns(*conn, table);
        if (expected.size() != actual.size()) {
            throw SchemaMismatchException("Schema mismatch for table '" + table + "': column count differs");
        }
        for (const auto& expected_column : expected) {
            const auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& actual_column) {
                return actual_column.name == expected_column.name;
            });
            if (found == actual.end()) {
                throw SchemaMismatchException("Schema mismatch for table '" + table +
                                              "': missing column '" + expected_column.name + "'");
            }
            require_equal(table, expected_column, *found);
        }
    }

    template<typename Entity, std::meta::info Member>
    static std::string join_table_sql(const SqlDialect& dialect) {
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;
        static_assert(detail::is_collection_relation_v<Field>,
                      "@ManyToMany fields must be std::vector<T> or LazyCollection<T>");
        using Related = typename detail::vector_value_type<Field>::type;
        static_assert(novaboot::di::detail::has_annotation<novaboot::annotations::JoinTable>(Member),
                      "@ManyToMany requires @JoinTable");
        constexpr auto join = novaboot::di::detail::get_annotation<novaboot::annotations::JoinTable>(Member);
        static_assert(join.name[0] != '\0' && join.join_column[0] != '\0' &&
                      join.inverse_join_column[0] != '\0',
                      "@JoinTable requires name, join_column, and inverse_join_column");

        return "CREATE TABLE IF NOT EXISTS " + std::string(join.name) + " (" +
            std::string(join.join_column) + " " + primary_key_type<Entity>(dialect) +
            " NOT NULL REFERENCES " + table_name<Entity>() + "(" + primary_key_column<Entity>() + "), " +
            std::string(join.inverse_join_column) + " " + primary_key_type<Related>(dialect) +
            " NOT NULL REFERENCES " + table_name<Related>() + "(" + primary_key_column<Related>() + "), " +
            "PRIMARY KEY (" + std::string(join.join_column) + ", " +
            std::string(join.inverse_join_column) + "))";
    }

    template<typename Entity, std::meta::info Member>
    static std::vector<ColumnSpec> join_table_columns(const SqlDialect& dialect) {
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;
        using Related = typename detail::vector_value_type<Field>::type;
        constexpr auto join = novaboot::di::detail::get_annotation<novaboot::annotations::JoinTable>(Member);
        const auto owner_type = primary_key_type<Entity>(dialect);
        const auto related_type = primary_key_type<Related>(dialect);
        return {
            ColumnSpec{
                .name = join.join_column,
                .type = canonical_type(owner_type),
                .length = canonical_type(owner_type) == "VARCHAR" ? type_length(owner_type) : -1,
                .nullable = false,
                .primary_key = true,
                .unique = false,
            },
            ColumnSpec{
                .name = join.inverse_join_column,
                .type = canonical_type(related_type),
                .length = canonical_type(related_type) == "VARCHAR" ? type_length(related_type) : -1,
                .nullable = false,
                .primary_key = true,
                .unique = false,
            },
        };
    }

    template<typename Entity>
    static void create_join_tables(DataSource& datasource) {
        auto dialect = datasource.dialect();
        auto conn = datasource.get_connection();
        static constexpr auto members = detail::get_members<Entity>();

        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::ManyToMany>(member)) {
                constexpr auto join = novaboot::di::detail::get_annotation<novaboot::annotations::JoinTable>(member);
                if (!table_exists(*conn, *dialect, join.name)) {
                    conn->execute(join_table_sql<Entity, member>(*dialect));
                }
                conn.reset();
                validate_columns(datasource, join.name, join_table_columns<Entity, member>(*dialect));
                conn = datasource.get_connection();
            }
        }
    }

public:
    template<typename Entity>
    static std::string create_table_sql(const SqlDialect& dialect) {
        static constexpr auto members = detail::get_members<Entity>();
        std::string columns;

        template for (constexpr auto member : members) {
            if constexpr (detail::is_persisted_entity_member<member>()) {
                if (!columns.empty()) columns += ", ";
                columns += column_sql<Entity, member>(dialect);
            }
        }
        if (columns.empty()) {
            throw std::invalid_argument("SchemaGenerator entity has no persisted fields");
        }
        return "CREATE TABLE IF NOT EXISTS " + table_name<Entity>() + " (" + columns + ")";
    }

    template<typename Entity>
    static void create_table(DataSource& datasource) {
        auto dialect = datasource.dialect();
        auto conn = datasource.get_connection();
        const auto table = table_name<Entity>();
        if (table_exists(*conn, *dialect, table)) {
            conn.reset();
            validate_table<Entity>(datasource);
            return;
        }
        conn->execute(create_table_sql<Entity>(*dialect));
        conn.reset();
        validate_table<Entity>(datasource);
    }

    template<typename Entity>
    static void validate_table(DataSource& datasource) {
        const auto table = table_name<Entity>();
        auto dialect = datasource.dialect();
        validate_columns(datasource, table, expected_columns<Entity>(*dialect));
        create_join_tables<Entity>(datasource);
    }
};

} // namespace novaboot::db
