#include <gtest/gtest.h>
#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/db/health.h"
#include "novaboot/db/observability.h"

TEST(DatabaseObservabilityTest, TimesOperationsWithoutSqlTextLabels) {
    auto meters = std::make_shared<novaboot::observability::MeterRegistry>();
    auto source = std::make_shared<novaboot::db::sqlite::SqliteDataSource>(":memory:", 1);
    novaboot::db::ObservingDataSource observed(source, meters, "sqlite");
    auto connection = observed.get_connection();
    connection->execute("CREATE TABLE entries (id INTEGER PRIMARY KEY, value TEXT)");
    connection->execute("INSERT INTO entries(value) VALUES (?)", {std::string("private-value")});
    EXPECT_EQ(connection->execute_batch(
                  "INSERT INTO entries(value) VALUES (?)",
                  {{std::string("batch-one")}, {std::string("batch-two")}}),
              (std::vector<std::int64_t>{1, 1}));
    auto rows = connection->query("SELECT value FROM entries");
    ASSERT_TRUE(rows->next());
    EXPECT_EQ(rows->get_string(0), "private-value");
    bool acquisition = false, insert = false, select = false;
    std::uint64_t insert_measurements = 0;
    for (const auto& metric : meters->snapshot()) {
        if (metric.name == "db.client.connection.acquire.duration") acquisition = true;
        if (metric.name == "db.client.operation.duration") {
            EXPECT_EQ(metric.attributes.at("db.system"), "sqlite");
            EXPECT_FALSE(metric.attributes.contains("db.query.text"));
            if (metric.attributes.at("db.operation.name") == "insert") {
                insert = true;
                insert_measurements += metric.count;
            }
            select |= metric.attributes.at("db.operation.name") == "select";
        }
    }
    EXPECT_TRUE(acquisition); EXPECT_TRUE(insert); EXPECT_TRUE(select);
    EXPECT_GE(insert_measurements, 2u); // single write plus one batch observation
}

TEST(DatabaseHealthContributorTest, ReportsAWorkingSqliteDataSourceAsUp) {
    novaboot::db::sqlite::SqliteDataSource source(":memory:", 1);
    const auto health = novaboot::db::health_contributor(source)();
    EXPECT_EQ(health.status, novaboot::actuator::HealthStatus::Up);
}
