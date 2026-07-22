#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include <spdlog/spdlog.h>
#include "novaboot/db/exceptions.h"
#include <stdexcept>
#include <format>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace {
std::string time_to_string(std::chrono::system_clock::time_point tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::chrono::system_clock::time_point string_to_time(const std::string& s) {
    std::tm tm = {};
    std::stringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

[[noreturn]] void throw_sqlite_error(int extended_code,
                                     const std::string& message,
                                     std::string_view prefix) {
    const auto full_message = std::string(prefix) + ": " + message;
    switch (extended_code) {
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY:
            throw novaboot::db::UniqueConstraintViolationException(full_message);
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            throw novaboot::db::ForeignKeyConstraintViolationException(full_message);
        case SQLITE_CONSTRAINT_NOTNULL:
            throw novaboot::db::NotNullConstraintViolationException(full_message);
        case SQLITE_CONSTRAINT:
        case SQLITE_CONSTRAINT_CHECK:
            throw novaboot::db::ConstraintViolationException(full_message);
        default:
            throw std::runtime_error(full_message);
    }
}
}

namespace novaboot::db::sqlite {

// ─── SqliteResultSet ─────────────────────────────────────────────────────────

SqliteResultSet::SqliteResultSet(sqlite3_stmt* stmt) : stmt_(stmt) {}

SqliteResultSet::~SqliteResultSet() {
    if (stmt_) {
        sqlite3_reset(stmt_);
    }
}

bool SqliteResultSet::next() {
    int rc = sqlite3_step(stmt_);
    has_next_ = (rc == SQLITE_ROW);
    return has_next_;
}

bool SqliteResultSet::is_null(int col_index) {
    return sqlite3_column_type(stmt_, col_index) == SQLITE_NULL;
}

std::int64_t SqliteResultSet::get_int(int col_index) {
    return sqlite3_column_int64(stmt_, col_index);
}

double SqliteResultSet::get_double(int col_index) {
    return sqlite3_column_double(stmt_, col_index);
}

std::string SqliteResultSet::get_string(int col_index) {
    const unsigned char* text = sqlite3_column_text(stmt_, col_index);
    return text ? reinterpret_cast<const char*>(text) : std::string();
}

std::vector<std::uint8_t> SqliteResultSet::get_blob(int col_index) {
    const void* data = sqlite3_column_blob(stmt_, col_index);
    int size = sqlite3_column_bytes(stmt_, col_index);
    if (!data || size <= 0) return {};
    const std::uint8_t* ptr = reinterpret_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(ptr, ptr + size);
}

bool SqliteResultSet::get_bool(int col_index) {
    return sqlite3_column_int(stmt_, col_index) != 0;
}

Uuid SqliteResultSet::get_uuid(int col_index) {
    return Uuid::from_string(get_string(col_index));
}

std::chrono::system_clock::time_point SqliteResultSet::get_time(int col_index) {
    return string_to_time(get_string(col_index));
}

int SqliteResultSet::column_count() const {
    return sqlite3_column_count(stmt_);
}

std::string_view SqliteResultSet::column_name(int col_index) const {
    const char* name = sqlite3_column_name(stmt_, col_index);
    return name ? std::string_view(name) : std::string_view();
}

// ─── SqliteConnection ────────────────────────────────────────────────────────

SqliteConnection::SqliteConnection(sqlite3* db, bool own_db) 
    : db_(db), own_db_(own_db) {}

SqliteConnection::~SqliteConnection() {
    if (own_db_ && db_) {
        sqlite3_close(db_);
    }
}

void SqliteConnection::bind_params(sqlite3_stmt* stmt, const std::vector<Parameter>& params) {
    for (size_t i = 0; i < params.size(); ++i) {
        int idx = static_cast<int>(i) + 1;
        std::visit([stmt, idx](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                sqlite3_bind_null(stmt, idx);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                sqlite3_bind_int64(stmt, idx, val);
            } else if constexpr (std::is_same_v<T, double>) {
                sqlite3_bind_double(stmt, idx, val);
            } else if constexpr (std::is_same_v<T, std::string>) {
                sqlite3_bind_text(stmt, idx, val.c_str(), -1, SQLITE_TRANSIENT);
            } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
                sqlite3_bind_blob(stmt, idx, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
            } else if constexpr (std::is_same_v<T, bool>) {
                sqlite3_bind_int(stmt, idx, val ? 1 : 0);
            } else if constexpr (std::is_same_v<T, Uuid>) {
                std::string s = val.to_string();
                sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT);
            } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                std::string s = time_to_string(val);
                sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT);
            }
        }, params[i]);
    }
}

std::int64_t SqliteConnection::execute(std::string_view sql, const std::vector<Parameter>& params) {
    log_query(sql, params);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.data(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw_sqlite_error(sqlite3_extended_errcode(db_),
                           std::format("{} (Query: {})", sqlite3_errmsg(db_), sql),
                           "SQL prepare failed");
    }

    bind_params(stmt, params);

    rc = sqlite3_step(stmt);
    const int extended_code = sqlite3_extended_errcode(db_);
    const std::string error_message = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw_sqlite_error(extended_code, error_message, "SQL execute failed");
    }
    return sqlite3_changes(db_);
}

std::vector<std::int64_t> SqliteConnection::execute_batch(
    std::string_view sql,
    const std::vector<std::vector<Parameter>>& parameter_sets) {
    std::vector<std::int64_t> affected_rows;
    affected_rows.reserve(parameter_sets.size());
    if (parameter_sets.empty()) return affected_rows;

    sqlite3_stmt* statement = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.data(), -1, &statement, nullptr);
    if (rc != SQLITE_OK) {
        throw_sqlite_error(sqlite3_extended_errcode(db_),
                           std::format("{} (Query: {})", sqlite3_errmsg(db_), sql),
                           "SQL batch prepare failed");
    }

    for (const auto& parameters : parameter_sets) {
        log_query(sql, parameters);
        sqlite3_clear_bindings(statement);
        bind_params(statement, parameters);

        rc = sqlite3_step(statement);
        const int extended_code = sqlite3_extended_errcode(db_);
        const std::string error_message = sqlite3_errmsg(db_);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw_sqlite_error(extended_code, error_message, "SQL batch execute failed");
        }
        affected_rows.push_back(sqlite3_changes(db_));

        rc = sqlite3_reset(statement);
        if (rc != SQLITE_OK) {
            const int reset_extended_code = sqlite3_extended_errcode(db_);
            const std::string reset_error_message = sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            throw_sqlite_error(reset_extended_code, reset_error_message, "SQL batch reset failed");
        }
    }

    sqlite3_finalize(statement);
    return affected_rows;
}

std::unique_ptr<ResultSet> SqliteConnection::query(std::string_view sql, const std::vector<Parameter>& params) {
    log_query(sql, params);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.data(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw_sqlite_error(sqlite3_extended_errcode(db_),
                           std::format("{} (Query: {})", sqlite3_errmsg(db_), sql),
                           "SQL query prepare failed");
    }

    bind_params(stmt, params);

    return std::make_unique<SqliteResultSet>(stmt);
}

std::int64_t SqliteConnection::last_insert_id() {
    return sqlite3_last_insert_rowid(db_);
}

void SqliteConnection::begin_transaction() {
    execute("BEGIN TRANSACTION;");
}

void SqliteConnection::commit() {
    execute("COMMIT;");
}

void SqliteConnection::rollback() {
    execute("ROLLBACK;");
}

// ─── SqliteDataSource ────────────────────────────────────────────────────────

SqliteDataSource::SqliteDataSource(std::string db_path, int pool_size,
                                   std::chrono::milliseconds acquisition_timeout,
                                   std::chrono::milliseconds leak_warning_threshold)
    : db_path_(std::move(db_path)), pool_size_(pool_size),
      acquisition_timeout_(acquisition_timeout),
      leak_warning_threshold_(leak_warning_threshold) {
    if (pool_size_ <= 0 || acquisition_timeout_ <= std::chrono::milliseconds::zero() ||
        leak_warning_threshold_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("SQLite pool size and timeouts must be positive");
    }
    for (int i = 0; i < pool_size_; ++i) {
        pool_->connections.push(create_connection());
    }
}

SqliteDataSource::~SqliteDataSource() {
    close();
}

sqlite3* SqliteDataSource::create_connection() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path_.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        throw std::runtime_error(std::format("Failed to open SQLite database: {}", db_path_));
    }
    // Enable WAL mode for concurrency
    sqlite3_extended_result_codes(db, 1);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    return db;
}

std::shared_ptr<Connection> SqliteDataSource::get_connection() {
    const auto pool = pool_;
    std::unique_lock<std::mutex> lock(pool->mutex);
    if (!pool->cv.wait_for(lock, acquisition_timeout_,
                           [&] { return !pool->connections.empty() || pool->closed; })) {
        throw std::runtime_error("SQLite connection pool acquisition timed out");
    }

    if (pool->closed) {
        throw std::runtime_error("DataSource is closed.");
    }

    sqlite3* db = pool->connections.front();
    pool->connections.pop();
    const auto leased_at = std::chrono::steady_clock::now();
    const auto leak_warning_threshold = leak_warning_threshold_;

    // Return a connection wrapper that returns the sqlite3 handle to the pool upon destruction
    auto deleter = [pool, db, leased_at, leak_warning_threshold](Connection* conn) {
        delete conn;
        const auto leased_for = std::chrono::steady_clock::now() - leased_at;
        if (leased_for >= leak_warning_threshold) {
            spdlog::warn("SQLite connection lease returned after {}ms (threshold {}ms)",
                         std::chrono::duration_cast<std::chrono::milliseconds>(leased_for).count(),
                         leak_warning_threshold.count());
        }
        std::lock_guard<std::mutex> return_lock(pool->mutex);
        if (!pool->closed) {
            pool->connections.push(db);
            pool->cv.notify_one();
        } else {
            sqlite3_close(db);
        }
    };

    return std::shared_ptr<Connection>(new SqliteConnection(db), deleter);
}

std::shared_ptr<SqlDialect> SqliteDataSource::dialect() {
    return dialect_;
}

void SqliteDataSource::close() {
    const auto pool = pool_;
    std::lock_guard<std::mutex> lock(pool->mutex);
    if (pool->closed) return;
    pool->closed = true;

    while (!pool->connections.empty()) {
        sqlite3* db = pool->connections.front();
        pool->connections.pop();
        sqlite3_close(db);
    }
    pool->cv.notify_all();
}

} // namespace novaboot::db::sqlite
