#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include <stdexcept>
#include <format>
#include <iostream>

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
            }
        }, params[i]);
    }
}

void SqliteConnection::execute(std::string_view sql, const std::vector<Parameter>& params) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.data(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::format("SQL prepare failed: {} (Query: {})", sqlite3_errmsg(db_), sql));
    }

    bind_params(stmt, params);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::format("SQL execute failed: {}", sqlite3_errmsg(db_)));
    }
}

std::unique_ptr<ResultSet> SqliteConnection::query(std::string_view sql, const std::vector<Parameter>& params) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.data(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::format("SQL query prepare failed: {} (Query: {})", sqlite3_errmsg(db_), sql));
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

SqliteDataSource::SqliteDataSource(std::string db_path, int pool_size)
    : db_path_(std::move(db_path)), pool_size_(pool_size) {
    for (int i = 0; i < pool_size_; ++i) {
        connections_.push(create_connection());
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
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return db;
}

std::shared_ptr<Connection> SqliteDataSource::get_connection() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return !connections_.empty() || closed_; });

    if (closed_) {
        throw std::runtime_error("DataSource is closed.");
    }

    sqlite3* db = connections_.front();
    connections_.pop();

    // Return a connection wrapper that returns the sqlite3 handle to the pool upon destruction
    auto deleter = [this, db](Connection* conn) {
        delete conn;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_) {
            connections_.push(db);
            cv_.notify_one();
        } else {
            sqlite3_close(db);
        }
    };

    return std::shared_ptr<Connection>(new SqliteConnection(db), deleter);
}

void SqliteDataSource::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    closed_ = true;

    while (!connections_.empty()) {
        sqlite3* db = connections_.front();
        connections_.pop();
        sqlite3_close(db);
    }
    cv_.notify_all();
}

} // namespace novaboot::db::sqlite
