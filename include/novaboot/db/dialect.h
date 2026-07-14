#pragma once
#include <string>
#include <string_view>
#include <memory>

namespace novaboot::db {

class SqlDialect {
public:
    virtual ~SqlDialect() = default;

    /// Return the name of the dialect
    virtual std::string_view name() const = 0;

    /// Convert database-agnostic '?' placeholders to dialect-specific symbols
    virtual std::string convert_placeholders(std::string_view sql) const = 0;

    /// Compile LIMIT and OFFSET clause for dialect
    virtual std::string compile_pagination(int limit, int offset) const = 0;
};

class SqliteDialect : public SqlDialect {
public:
    std::string_view name() const override { return "sqlite"; }

    std::string convert_placeholders(std::string_view sql) const override {
        return std::string(sql); // SQLite uses native '?' placeholders
    }

    std::string compile_pagination(int limit, int offset) const override {
        std::string out;
        if (limit > 0) {
            out += " LIMIT " + std::to_string(limit);
        }
        if (offset > 0) {
            out += " OFFSET " + std::to_string(offset);
        }
        return out;
    }
};

class PostgresDialect : public SqlDialect {
public:
    std::string_view name() const override { return "postgresql"; }

    std::string convert_placeholders(std::string_view sql) const override {
        std::string out;
        out.reserve(sql.size() + 10);
        int count = 1;
        for (size_t i = 0; i < sql.size(); ++i) {
            if (sql[i] == '?') {
                out += "$" + std::to_string(count++);
            } else {
                out += sql[i];
            }
        }
        return out;
    }

    std::string compile_pagination(int limit, int offset) const override {
        std::string out;
        if (limit > 0) {
            out += " LIMIT " + std::to_string(limit);
        }
        if (offset > 0) {
            out += " OFFSET " + std::to_string(offset);
        }
        return out;
    }
};

} // namespace novaboot::db
