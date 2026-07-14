#include <gtest/gtest.h>
#include "novaboot/db/uuid.h"
#include "novaboot/db/dialect.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/db/drivers/postgres/postgres_driver.h"
#include <thread>
#include <chrono>
#include <iostream>

using namespace novaboot::db;
using namespace novaboot::annotations;

TEST(Uuidv7Test, OrderingAndParsing) {
    auto u1 = Uuid::generate();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto u2 = Uuid::generate();

    EXPECT_FALSE(u1.is_nil());
    EXPECT_FALSE(u2.is_nil());
    EXPECT_NE(u1, u2);
    EXPECT_LT(u1, u2); // Chronological ordering check

    std::string s1 = u1.to_string();
    Uuid parsed = Uuid::from_string(s1);
    EXPECT_EQ(u1, parsed);
}

TEST(DialectTest, PlaceholderConversion) {
    SqliteDialect sqlite;
    PostgresDialect postgres;

    std::string q = "SELECT * FROM users WHERE name = ? AND email = ?";
    EXPECT_EQ(sqlite.convert_placeholders(q), "SELECT * FROM users WHERE name = ? AND email = ?");
    EXPECT_EQ(postgres.convert_placeholders(q), "SELECT * FROM users WHERE name = $1 AND email = $2");
}

struct [[= Entity("portable_tests") ]] PortableEntity {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::UUID) ]]
    std::string id;

    Uuid uuid_field;
    std::chrono::system_clock::time_point time_field;
    std::vector<std::uint8_t> blob_field;
};

struct PortableRepository : public CrudRepository<PortableEntity, std::string> {
    explicit PortableRepository(std::shared_ptr<DataSource> ds)
        : CrudRepository<PortableEntity, std::string>(ds) {}
};

TEST(SQLitePortableTest, CRUD) {
    auto ds = std::make_shared<novaboot::db::sqlite::SqliteDataSource>(":memory:", 1);
    {
        auto conn = ds->get_connection();
        conn->execute(R"(
            CREATE TABLE portable_tests (
                id TEXT PRIMARY KEY,
                uuid_field TEXT,
                time_field TEXT,
                blob_field BLOB
            );
        )");
    }

    PortableRepository repo(ds);
    auto now = std::chrono::system_clock::now();
    
    PortableEntity entity{};
    entity.uuid_field = Uuid::generate();
    entity.time_field = now;
    entity.blob_field = {0x01, 0x02, 0x03, 0x04};

    auto saved = repo.save(entity);
    EXPECT_FALSE(saved.id.empty()); // Generated UUIDv7

    auto retrieved = repo.find_by_id(saved.id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->uuid_field, entity.uuid_field);
    EXPECT_EQ(retrieved->blob_field, entity.blob_field);

    // Verify time parsing: conversion to/from string loses sub-second precision in standard mktime
    // so we compare within 1 second.
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(retrieved->time_field - entity.time_field).count();
    EXPECT_NEAR(diff, 0, 1);

    // Verify Dialect Pagination
    auto list = repo.query().limit(5).offset(0).list();
    EXPECT_EQ(list.size(), 1);
}

TEST(PostgresPortableTest, CRUD) {
    std::string conn_info = "host=localhost dbname=postgres user=postgres password=postgres connect_timeout=2";
    std::shared_ptr<novaboot::db::postgres::PostgresDataSource> ds;
    try {
        ds = std::make_shared<novaboot::db::postgres::PostgresDataSource>(conn_info, 1);
    } catch (...) {
        GTEST_SKIP() << "Postgres server not reachable. Skipping.";
        return;
    }

    try {
        auto conn = ds->get_connection();
        conn->execute("DROP TABLE IF EXISTS portable_tests;");
        conn->execute(R"(
            CREATE TABLE portable_tests (
                id VARCHAR(50) PRIMARY KEY,
                uuid_field VARCHAR(50),
                time_field TIMESTAMP,
                blob_field BYTEA
            );
        )");
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Postgres configuration error: " << e.what() << ". Skipping.";
        return;
    }

    PortableRepository repo(ds);
    auto now = std::chrono::system_clock::now();

    PortableEntity entity{};
    entity.uuid_field = Uuid::generate();
    entity.time_field = now;
    entity.blob_field = {0xAA, 0xBB, 0xCC};

    auto saved = repo.save(entity);
    EXPECT_FALSE(saved.id.empty());

    auto retrieved = repo.find_by_id(saved.id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->uuid_field, entity.uuid_field);

    auto diff = std::chrono::duration_cast<std::chrono::seconds>(retrieved->time_field - entity.time_field).count();
    EXPECT_NEAR(diff, 0, 1);

    // Cleanup
    {
        auto conn = ds->get_connection();
        conn->execute("DROP TABLE IF EXISTS portable_tests;");
    }
}
