#pragma once
#include "novaboot/db/db_client.h"
#include <libpq-fe.h>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace novaboot::db::postgres {

class PostgresResultSet : public ResultSet {
private:
    PGresult* res_ = nullptr;
    int row_count_ = 0;
    int current_row_ = -1;

public:
    explicit PostgresResultSet(PGresult* res);
    ~PostgresResultSet() override;

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

class PostgresConnection : public Connection {
private:
    PGconn* conn_ = nullptr;
    bool own_conn_ = false;
    std::int64_t last_inserted_id_ = 0;

    std::string convert_placeholders(std::string_view sql);
    std::vector<std::string> serialize_params(const std::vector<Parameter>& params);

public:
    explicit PostgresConnection(PGconn* conn, bool own_conn = false);
    ~PostgresConnection() override;

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

class PostgresDataSource : public DataSource {
private:
    struct PoolState {
        std::queue<PGconn*> connections;
        std::mutex mutex;
        std::condition_variable cv;
        bool closed = false;
    };

    std::string conn_info_;
    int pool_size_ = 1;
    std::chrono::milliseconds acquisition_timeout_{30000};
    std::chrono::milliseconds leak_warning_threshold_{60000};
    std::string startup_sql_;
    std::shared_ptr<PoolState> pool_ = std::make_shared<PoolState>();
    std::shared_ptr<SqlDialect> dialect_ = std::make_shared<PostgresDialect>();

    PGconn* create_connection();

public:
    /// `startup_sql` runs once on every newly opened physical connection.
    /// It is useful for connection-local settings such as PostgreSQL search_path.
    explicit PostgresDataSource(std::string conn_info, int pool_size = 4,
                                std::chrono::milliseconds acquisition_timeout = std::chrono::seconds{30},
                                std::chrono::milliseconds leak_warning_threshold = std::chrono::seconds{60},
                                std::string startup_sql = {});
    ~PostgresDataSource() override;

    std::shared_ptr<Connection> get_connection() override;
    std::shared_ptr<SqlDialect> dialect() override;
    void close() override;
};

} // namespace novaboot::db::postgres
