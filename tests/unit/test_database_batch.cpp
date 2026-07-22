#include <gtest/gtest.h>

#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/db/exceptions.h"

namespace {

using novaboot::db::Parameter;

int count_insert_preparations(void* context, int action, const char* table,
                              const char* /*column*/, const char* /*database*/,
                              const char* /*trigger*/) {
    if (action == SQLITE_INSERT && table != nullptr && std::string_view(table) == "messages") {
        ++*static_cast<int*>(context);
    }
    return SQLITE_OK;
}

TEST(DatabaseBatchTest, SqliteReusesOneStatementForSeveralParameterSets) {
    novaboot::db::sqlite::SqliteDataSource source(":memory:", 1);
    auto connection = source.get_connection();
    connection->execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, body TEXT NOT NULL)");

    const auto affected = connection->execute_batch(
        "INSERT INTO messages (id, body) VALUES (?, ?)",
        {{Parameter{std::int64_t{1}}, Parameter{std::string{"first"}}},
         {Parameter{std::int64_t{2}}, Parameter{std::string{"second"}}},
         {Parameter{std::int64_t{3}}, Parameter{std::string{"third"}}}});

    EXPECT_EQ(affected, (std::vector<std::int64_t>{1, 1, 1}));
    auto rows = connection->query("SELECT id, body FROM messages ORDER BY id");
    ASSERT_TRUE(rows->next());
    EXPECT_EQ(rows->get_int(0), 1);
    EXPECT_EQ(rows->get_string(1), "first");
    ASSERT_TRUE(rows->next());
    EXPECT_EQ(rows->get_int(0), 2);
    EXPECT_EQ(rows->get_string(1), "second");
    ASSERT_TRUE(rows->next());
    EXPECT_EQ(rows->get_int(0), 3);
    EXPECT_EQ(rows->get_string(1), "third");
    EXPECT_FALSE(rows->next());
}

TEST(DatabaseBatchTest, BatchFailureLeavesTransactionControlWithTheCaller) {
    novaboot::db::sqlite::SqliteDataSource source(":memory:", 1);
    auto connection = source.get_connection();
    connection->execute("CREATE TABLE unique_values (value INTEGER UNIQUE)");

    connection->begin_transaction();
    EXPECT_THROW(
        connection->execute_batch(
            "INSERT INTO unique_values (value) VALUES (?)",
            {{Parameter{std::int64_t{1}}}, {Parameter{std::int64_t{1}}}}),
        novaboot::db::UniqueConstraintViolationException);
    connection->rollback();

    auto rows = connection->query("SELECT COUNT(*) FROM unique_values");
    ASSERT_TRUE(rows->next());
    EXPECT_EQ(rows->get_int(0), 0);
}

TEST(DatabaseBatchTest, EmptyBatchDoesNotExecuteTheStatement) {
    novaboot::db::sqlite::SqliteDataSource source(":memory:", 1);
    auto connection = source.get_connection();

    EXPECT_TRUE(connection->execute_batch("not valid SQL", {}).empty());
}

TEST(DatabaseBatchTest, SqliteBatchPreparesTheStatementOnce) {
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
    novaboot::db::sqlite::SqliteConnection connection(raw, true);
    connection.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, body TEXT NOT NULL)");

    int prepared_inserts = 0;
    ASSERT_EQ(sqlite3_set_authorizer(raw, count_insert_preparations, &prepared_inserts), SQLITE_OK);
    const std::vector<std::vector<Parameter>> values{
        {Parameter{std::int64_t{1}}, Parameter{std::string{"one"}}},
        {Parameter{std::int64_t{2}}, Parameter{std::string{"two"}}},
        {Parameter{std::int64_t{3}}, Parameter{std::string{"three"}}}};

    connection.execute_batch("INSERT INTO messages (id, body) VALUES (?, ?)", values);
    EXPECT_EQ(prepared_inserts, 1);

    connection.execute("DELETE FROM messages");
    prepared_inserts = 0;
    for (const auto& parameters : values) {
        connection.execute("INSERT INTO messages (id, body) VALUES (?, ?)", parameters);
    }
    EXPECT_EQ(prepared_inserts, 3);
}

} // namespace
