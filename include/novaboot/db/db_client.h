#pragma once
#include "novaboot/db/uuid.h"
#include "novaboot/db/dialect.h"
#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <memory>
#include <optional>
#include <cstdint>
#include <chrono>
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>

namespace novaboot::db {

/// Database bind parameter type variant
using Parameter = std::variant<
    std::nullptr_t,
    std::int64_t,
    double,
    std::string,
    std::vector<std::uint8_t>,
    bool,
    Uuid,
    std::chrono::system_clock::time_point
>;

inline bool show_sql = false;

inline std::string format_parameter(const Parameter& param) {
    return std::visit([](auto&& val) -> std::string {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            return "NULL";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::to_string(val);
        } else if constexpr (std::is_same_v<T, double>) {
            return std::to_string(val);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return "'" + val + "'";
        } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
            return "<blob of " + std::to_string(val.size()) + " bytes>";
        } else if constexpr (std::is_same_v<T, bool>) {
            return val ? "true" : "false";
        } else if constexpr (std::is_same_v<T, Uuid>) {
            return "'" + val.to_string() + "'";
        } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
            auto time = std::chrono::system_clock::to_time_t(val);
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
            return "'" + ss.str() + "'";
        }
    }, param);
}

inline void log_query(std::string_view sql, const std::vector<Parameter>& params) {
    if (!show_sql) return;
    std::string sql_str(sql);
    for (auto& c : sql_str) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string clean_sql;
    bool last_was_space = false;
    for (char c : sql_str) {
        if (std::isspace(c)) {
            if (!last_was_space) {
                clean_sql += ' ';
                last_was_space = true;
            }
        } else {
            clean_sql += c;
            last_was_space = false;
        }
    }
    // Trim leading/trailing spaces
    if (!clean_sql.empty() && clean_sql.front() == ' ') {
        clean_sql.erase(clean_sql.begin());
    }
    if (!clean_sql.empty() && clean_sql.back() == ' ') {
        clean_sql.pop_back();
    }

    if (params.empty()) {
        spdlog::info("NovaBoot SQL: {}", clean_sql);
    } else {
        std::string params_str;
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) params_str += ", ";
            params_str += format_parameter(params[i]);
        }
        spdlog::info("NovaBoot SQL: {} [Params: {}]", clean_sql, params_str);
    }
}

/// Abstract Row Result Set
class ResultSet {
public:
    virtual ~ResultSet() = default;

    /// Advance to the next row. Returns false when no more rows exist.
    virtual bool next() = 0;

    /// Check if the column is null
    virtual bool is_null(int col_index) = 0;

    /// Get column value by index
    virtual std::int64_t get_int(int col_index) = 0;
    virtual double get_double(int col_index) = 0;
    virtual std::string get_string(int col_index) = 0;
    virtual std::vector<std::uint8_t> get_blob(int col_index) = 0;
    virtual bool get_bool(int col_index) = 0;
    virtual Uuid get_uuid(int col_index) = 0;
    virtual std::chrono::system_clock::time_point get_time(int col_index) = 0;

    /// Get metadata info
    virtual int column_count() const = 0;
    virtual std::string_view column_name(int col_index) const = 0;
};

/// Abstract Connection Session
class Connection {
public:
    virtual ~Connection() = default;

    /// Execute write queries (INSERT, UPDATE, DELETE, CREATE TABLE) returning affected rows count
    virtual std::int64_t execute(std::string_view sql, const std::vector<Parameter>& params = {}) = 0;

    /// Execute the same statement for several parameter sets.
    ///
    /// The default preserves the semantics of calling execute() for each row.
    /// Drivers may override it to reuse a prepared statement or use their native
    /// batch protocol.  It does not create a transaction: callers that require
    /// all-or-nothing behaviour must supply their own transaction boundary.
    virtual std::vector<std::int64_t> execute_batch(
        std::string_view sql,
        const std::vector<std::vector<Parameter>>& parameter_sets) {
        std::vector<std::int64_t> affected_rows;
        affected_rows.reserve(parameter_sets.size());
        for (const auto& parameters : parameter_sets) {
            affected_rows.push_back(execute(sql, parameters));
        }
        return affected_rows;
    }

    /// Execute read queries (SELECT) returning a ResultSet
    virtual std::unique_ptr<ResultSet> query(std::string_view sql, const std::vector<Parameter>& params = {}) = 0;

    /// Get the ID generated by the last INSERT operation
    virtual std::int64_t last_insert_id() = 0;

    /// Transaction Controls
    virtual void begin_transaction() = 0;
    virtual void commit() = 0;
    virtual void rollback() = 0;

    /// Make driver-buffered work visible to the database.
    ///
    /// NovaBoot repositories execute writes eagerly and do not maintain a
    /// Hibernate-style persistence context, so the base implementation is a
    /// no-op. A future asynchronous/buffered driver may override this method.
    virtual void flush() {}
};

/// Abstract connection pool (DataSource)
class DataSource {
public:
    virtual ~DataSource() = default;

    /// Retrieve an active connection from the pool
    virtual std::shared_ptr<Connection> get_connection() = 0;

    /// Get the database dialect
    virtual std::shared_ptr<SqlDialect> dialect() = 0;

    /// Graceful shutdown
    virtual void close() = 0;
};

} // namespace novaboot::db
