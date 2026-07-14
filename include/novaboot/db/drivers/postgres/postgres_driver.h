#pragma once
#include "novaboot/db/db_client.h"
#include <libpq-fe.h>
#include <mutex>
#include <queue>
#include <condition_variable>
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

    void execute(std::string_view sql, const std::vector<Parameter>& params = {}) override;
    std::unique_ptr<ResultSet> query(std::string_view sql, const std::vector<Parameter>& params = {}) override;
    
    std::int64_t last_insert_id() override;

    void begin_transaction() override;
    void commit() override;
    void rollback() override;
};

class PostgresDataSource : public DataSource {
private:
    std::string conn_info_;
    int pool_size_ = 1;
    std::queue<PGconn*> connections_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;

    PGconn* create_connection();

public:
    explicit PostgresDataSource(std::string conn_info, int pool_size = 4);
    ~PostgresDataSource() override;

    std::shared_ptr<Connection> get_connection() override;
    void close() override;
};

} // namespace novaboot::db::postgres
