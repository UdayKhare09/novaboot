#pragma once

#include <chrono>
#include <cctype>
#include <memory>

#include "novaboot/db/db_client.h"
#include "novaboot/observability/observation.h"

namespace novaboot::db {

/// Opt-in DataSource decorator that emits low-cardinality database timings.
class ObservingConnection final : public Connection {
public:
    ObservingConnection(std::shared_ptr<Connection> delegate,
                        std::shared_ptr<observability::MeterRegistry> meters,
                        std::string system)
        : delegate_(std::move(delegate)), meters_(std::move(meters)), system_(std::move(system)) {}

    std::int64_t execute(std::string_view sql, const std::vector<Parameter>& params = {}) override {
        const auto started = std::chrono::steady_clock::now();
        try {
            const auto affected = delegate_->execute(sql, params);
            record(sql, "ok", started);
            return affected;
        } catch (...) {
            record(sql, "error", started);
            throw;
        }
    }
    std::vector<std::int64_t> execute_batch(
        std::string_view sql,
        const std::vector<std::vector<Parameter>>& parameter_sets) override {
        const auto started = std::chrono::steady_clock::now();
        try {
            auto affected = delegate_->execute_batch(sql, parameter_sets);
            record(sql, "ok", started);
            return affected;
        } catch (...) {
            record(sql, "error", started);
            throw;
        }
    }
    std::unique_ptr<ResultSet> query(std::string_view sql, const std::vector<Parameter>& params = {}) override {
        const auto started = std::chrono::steady_clock::now();
        try {
            auto result = delegate_->query(sql, params);
            record(sql, "ok", started);
            return result;
        } catch (...) {
            record(sql, "error", started);
            throw;
        }
    }
    std::int64_t last_insert_id() override { return delegate_->last_insert_id(); }
    void begin_transaction() override { delegate_->begin_transaction(); }
    void commit() override { delegate_->commit(); }
    void rollback() override { delegate_->rollback(); }
    void flush() override { delegate_->flush(); }

private:
    static std::string operation(std::string_view sql) {
        while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.front()))) sql.remove_prefix(1);
        const auto end = sql.find_first_of(" \t\r\n");
        std::string result(sql.substr(0, end));
        for (auto& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return result.empty() ? "other" : result;
    }
    void record(std::string_view sql, std::string_view outcome,
                std::chrono::steady_clock::time_point started) const {
        meters_->histogram_record("db.client.operation.duration",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), "s",
            {{"db.system", system_}, {"db.operation.name", operation(sql)}, {"error.type", std::string(outcome)}});
    }

    std::shared_ptr<Connection> delegate_;
    std::shared_ptr<observability::MeterRegistry> meters_;
    std::string system_;
};

class ObservingDataSource final : public DataSource {
public:
    ObservingDataSource(std::shared_ptr<DataSource> delegate,
                        std::shared_ptr<observability::MeterRegistry> meters,
                        std::string system)
        : delegate_(std::move(delegate)), meters_(std::move(meters)), system_(std::move(system)) {}
    std::shared_ptr<Connection> get_connection() override {
        const auto started = std::chrono::steady_clock::now();
        auto connection = delegate_->get_connection();
        meters_->histogram_record("db.client.connection.acquire.duration",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), "s",
            {{"db.system", system_}});
        return std::make_shared<ObservingConnection>(std::move(connection), meters_, system_);
    }
    std::shared_ptr<SqlDialect> dialect() override { return delegate_->dialect(); }
    void close() override { delegate_->close(); }
private:
    std::shared_ptr<DataSource> delegate_;
    std::shared_ptr<observability::MeterRegistry> meters_;
    std::string system_;
};

} // namespace novaboot::db
