#include <benchmark/benchmark.h>

#include "novaboot/db/drivers/sqlite/sqlite_driver.h"

namespace {

constexpr std::int64_t batch_size = 100;

std::vector<std::vector<novaboot::db::Parameter>> rows() {
    std::vector<std::vector<novaboot::db::Parameter>> parameters;
    parameters.reserve(static_cast<std::size_t>(batch_size));
    for (std::int64_t id = 1; id <= batch_size; ++id) {
        parameters.push_back({id, std::string("message-") + std::to_string(id)});
    }
    return parameters;
}

class SqliteBatchFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        source_ = std::make_unique<novaboot::db::sqlite::SqliteDataSource>(":memory:", 1);
        connection_ = source_->get_connection();
        connection_->execute(
            "CREATE TABLE benchmark_messages (id INTEGER PRIMARY KEY, body TEXT NOT NULL)");
        rows_ = rows();
    }

    void TearDown(const benchmark::State&) override {
        connection_.reset();
        source_.reset();
    }

protected:
    void clear(benchmark::State& state) {
        state.PauseTiming();
        connection_->execute("DELETE FROM benchmark_messages");
        state.ResumeTiming();
    }

    std::unique_ptr<novaboot::db::sqlite::SqliteDataSource> source_;
    std::shared_ptr<novaboot::db::Connection> connection_;
    std::vector<std::vector<novaboot::db::Parameter>> rows_;
};

BENCHMARK_DEFINE_F(SqliteBatchFixture, ExecuteOneByOne)(benchmark::State& state) {
    for (auto _ : state) {
        clear(state);
        for (const auto& parameters : rows_) {
            benchmark::DoNotOptimize(connection_->execute(
                "INSERT INTO benchmark_messages (id, body) VALUES (?, ?)", parameters));
        }
    }
    state.SetItemsProcessed(state.iterations() * batch_size);
}

BENCHMARK_DEFINE_F(SqliteBatchFixture, ExecuteBatch)(benchmark::State& state) {
    for (auto _ : state) {
        clear(state);
        benchmark::DoNotOptimize(connection_->execute_batch(
            "INSERT INTO benchmark_messages (id, body) VALUES (?, ?)", rows_));
    }
    state.SetItemsProcessed(state.iterations() * batch_size);
}

BENCHMARK_REGISTER_F(SqliteBatchFixture, ExecuteOneByOne);
BENCHMARK_REGISTER_F(SqliteBatchFixture, ExecuteBatch);

} // namespace

BENCHMARK_MAIN();
