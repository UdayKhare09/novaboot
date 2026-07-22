#include <gtest/gtest.h>

#include "novaboot/db/drivers/sqlite/sqlite_driver.h"

#include <chrono>

TEST(ConnectionPoolTest, TimesOutWhenAllSqliteConnectionsAreLeased) {
    using namespace std::chrono_literals;
    novaboot::db::sqlite::SqliteDataSource source(":memory:", 1, 10ms, 1s);
    auto first = source.get_connection();

    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW((void)source.get_connection(), std::runtime_error);
    EXPECT_GE(std::chrono::steady_clock::now() - started, 8ms);

    first.reset();
    EXPECT_NO_THROW((void)source.get_connection());
}

TEST(ConnectionPoolTest, ClosingDatasourceSafelyClosesReturnedLease) {
    novaboot::db::sqlite::SqliteDataSource source(":memory:", 1);
    auto connection = source.get_connection();
    source.close();
    EXPECT_NO_THROW(connection.reset());
    EXPECT_THROW((void)source.get_connection(), std::runtime_error);
}
