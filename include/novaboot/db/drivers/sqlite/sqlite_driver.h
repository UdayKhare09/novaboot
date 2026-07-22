#pragma once
#include "novaboot/db/db_client.h"
#include <sqlite3.h>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace novaboot::db::sqlite {

class SqliteResultSet : public ResultSet {
private:
    sqlite3_stmt* stmt_ = nullptr;
    bool has_next_ = false;

public:
    explicit SqliteResultSet(sqlite3_stmt* stmt);
    ~SqliteResultSet() override;

    bool next() override;
    bool is_null(int col_index) override;
    std::int64_t get_int(int col_index) override;
    double get_double(int col_index) override;
    std::string get_string(int col_index) override;
    std::vector<std::uint8_t> get_blob(int col_index) override;
    bool get_bool(int col_index) override;
    Uuid get_uuid(int col_index) override;
    std::chrono::system_clock::time_point get_time(int col_index) override;
    int column_count() const override;
    std::string_view column_name(int col_index) const override;
};

class SqliteConnection : public Connection {
private:
    sqlite3* db_ = nullptr;
    bool own_db_ = false;

    void bind_params(sqlite3_stmt* stmt, const std::vector<Parameter>& params);

public:
    explicit SqliteConnection(sqlite3* db, bool own_db = false);
    ~SqliteConnection() override;

    std::int64_t execute(std::string_view sql, const std::vector<Parameter>& params = {}) override;
    std::vector<std::int64_t> execute_batch(
        std::string_view sql,
        const std::vector<std::vector<Parameter>>& parameter_sets) override;
    std::unique_ptr<ResultSet> query(std::string_view sql, const std::vector<Parameter>& params = {}) override;
    
    std::int64_t last_insert_id() override;

    void begin_transaction() override;
    void commit() override;
    void rollback() override;
};

class SqliteDataSource : public DataSource {
private:
    struct PoolState {
        std::queue<sqlite3*> connections;
        std::mutex mutex;
        std::condition_variable cv;
        bool closed = false;
    };

    std::string db_path_;
    int pool_size_ = 1;
    std::chrono::milliseconds acquisition_timeout_{30000};
    std::chrono::milliseconds leak_warning_threshold_{60000};
    std::shared_ptr<PoolState> pool_ = std::make_shared<PoolState>();
    std::shared_ptr<SqlDialect> dialect_ = std::make_shared<SqliteDialect>();

    sqlite3* create_connection();

public:
    explicit SqliteDataSource(std::string db_path, int pool_size = 4,
                              std::chrono::milliseconds acquisition_timeout = std::chrono::seconds{30},
                              std::chrono::milliseconds leak_warning_threshold = std::chrono::seconds{60});
    ~SqliteDataSource() override;

    std::shared_ptr<Connection> get_connection() override;
    std::shared_ptr<SqlDialect> dialect() override;
    void close() override;
};

} // namespace novaboot::db::sqlite
