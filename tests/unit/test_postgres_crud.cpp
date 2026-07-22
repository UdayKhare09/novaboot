#include <gtest/gtest.h>
#include "novaboot/db/drivers/postgres/postgres_driver.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/schema.h"
#include "novaboot/db/transaction.h"
#include "novaboot/db/drivers/postgres/postgres_session_store.h"
#include "support/postgres_test_database.h"
#include <vector>
#include <string>
#include <iostream>

using namespace novaboot;
using namespace novaboot::db;
using namespace novaboot::db::postgres;
using namespace novaboot::annotations;

struct [[= Entity("test_postgres_entities") ]] DBPostgresTestEntity {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;
    
    std::string name;
    int score = 0;
};

struct PostgresTestEntityRepository : public CrudRepository<DBPostgresTestEntity, int> {
    explicit PostgresTestEntityRepository(std::shared_ptr<DataSource> ds) 
        : CrudRepository<DBPostgresTestEntity, int>(ds) {}
        
    std::vector<DBPostgresTestEntity> find_by_score_above(int score) {
        return query()
            .where<&DBPostgresTestEntity::score>(Op::GreaterThan, score)
            .order_by<&DBPostgresTestEntity::score>(false /* desc */)
            .list();
    }
};

struct [[= Entity("test_postgres_versioned_entities") ]] VersionedPostgresEntity {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;

    [[= Column("title", false) ]]
    std::string title;

    [[= Version() ]]
    int version = 0;
};

struct VersionedPostgresRepository : public CrudRepository<VersionedPostgresEntity, int> {
    explicit VersionedPostgresRepository(std::shared_ptr<DataSource> ds,
                                         std::shared_ptr<Connection> connection = nullptr)
        : CrudRepository<VersionedPostgresEntity, int>(std::move(ds), std::move(connection)) {}
};

TEST(PostgresCrudTest, LifecycleAndQuery) {
    std::unique_ptr<novaboot::testing::PostgresTestDatabase> database;
    try {
        database = std::make_unique<novaboot::testing::PostgresTestDatabase>();
    } catch (const std::exception& e) {
        std::cout << "[WARNING] PostgreSQL not available: " << e.what() << "\n";
        std::cout << "[INFO] Skipping PostgreSQL CRUD integration tests.\n";
        GTEST_SKIP() << "PostgreSQL server not available.";
        return;
    }
    auto ds = database->datasource();
    
    auto conn = ds->get_connection();
    try {
        conn->execute("DROP TABLE IF EXISTS test_postgres_entities;");
        conn->execute(R"(
            CREATE TABLE test_postgres_entities (
                id SERIAL PRIMARY KEY,
                name TEXT NOT NULL,
                score INTEGER NOT NULL
            );
        )");
    } catch (const std::exception& e) {
        std::cout << "[WARNING] Failed to drop/create table: " << e.what() << "\n";
        return;
    }
    
    PostgresTestEntityRepository repo(ds);
    
    // 1. Save new entities
    DBPostgresTestEntity e1{0, "Bob", 85};
    DBPostgresTestEntity e2{0, "Alice", 95};
    
    auto s1 = repo.save(e1);
    auto s2 = repo.save(e2);
    
    // Verify auto-increment IDs populated
    EXPECT_EQ(s1.id, 1);
    EXPECT_EQ(s2.id, 2);
    
    // 2. Find by id
    auto f1 = repo.find_by_id(1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->name, "Bob");
    EXPECT_EQ(f1->score, 85);
    
    // 3. Exists by id
    EXPECT_TRUE(repo.exists_by_id(2));
    EXPECT_FALSE(repo.exists_by_id(3));
    
    // 4. Update
    s1.score = 90;
    repo.save(s1);
    auto f1_updated = repo.find_by_id(1);
    ASSERT_TRUE(f1_updated.has_value());
    EXPECT_EQ(f1_updated->score, 90);
    
    // 5. Custom Query / Sort / Limit
    auto matches = repo.find_by_score_above(88);
    ASSERT_EQ(matches.size(), 2);
    EXPECT_EQ(matches[0].name, "Alice");
    EXPECT_EQ(matches[1].name, "Bob");
    
    // 6. Delete
    repo.delete_by_id(1);
    EXPECT_FALSE(repo.exists_by_id(1));
    EXPECT_TRUE(repo.exists_by_id(2));
    
    // Clean up
    conn->execute("DROP TABLE IF EXISTS test_postgres_entities;");
}

TEST(PostgresCrudTest, MapsConstraintViolationsToPortableExceptions) {
    std::unique_ptr<novaboot::testing::PostgresTestDatabase> database;
    try {
        database = std::make_unique<novaboot::testing::PostgresTestDatabase>();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "PostgreSQL server not available: " << e.what();
    }
    auto ds = database->datasource();

    auto conn = ds->get_connection();
    conn->execute("DROP TABLE IF EXISTS test_pg_constraint_children;");
    conn->execute("DROP TABLE IF EXISTS test_pg_constraint_parents;");
    conn->execute("CREATE TABLE test_pg_constraint_parents (id INTEGER PRIMARY KEY)");
    conn->execute(R"(
        CREATE TABLE test_pg_constraint_children (
            id INTEGER PRIMARY KEY,
            parent_id INTEGER NOT NULL REFERENCES test_pg_constraint_parents(id),
            code TEXT NOT NULL UNIQUE
        )
    )");
    conn->execute("INSERT INTO test_pg_constraint_parents (id) VALUES (1)");
    conn->execute("INSERT INTO test_pg_constraint_children (id, parent_id, code) VALUES (1, 1, 'a')");

    EXPECT_THROW(conn->execute(
        "INSERT INTO test_pg_constraint_children (id, parent_id, code) VALUES (2, 1, 'a')"),
        UniqueConstraintViolationException);

    EXPECT_THROW(conn->execute(
        "INSERT INTO test_pg_constraint_children (id, parent_id, code) VALUES (3, 1, NULL)"),
        NotNullConstraintViolationException);

    EXPECT_THROW(conn->execute(
        "INSERT INTO test_pg_constraint_children (id, parent_id, code) VALUES (4, 404, 'b')"),
        ForeignKeyConstraintViolationException);

    conn->execute("DROP TABLE IF EXISTS test_pg_constraint_children;");
    conn->execute("DROP TABLE IF EXISTS test_pg_constraint_parents;");
}

TEST(PostgresCrudTest, ExecutesUniformSqlAsOnePreparedPipelineBatch) {
    std::unique_ptr<novaboot::testing::PostgresTestDatabase> database;
    try {
        database = std::make_unique<novaboot::testing::PostgresTestDatabase>();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "PostgreSQL server not available: " << e.what();
    }
    auto ds = database->datasource();

    auto conn = ds->get_connection();
    conn->execute("DROP TABLE IF EXISTS test_pg_batch_values;");
    conn->execute("CREATE TABLE test_pg_batch_values (id INTEGER PRIMARY KEY, value TEXT NOT NULL)");

    const auto affected = conn->execute_batch(
        "INSERT INTO test_pg_batch_values (id, value) VALUES (?, ?)",
        {{Parameter{std::int64_t{1}}, Parameter{std::string{"one"}}},
         {Parameter{std::int64_t{2}}, Parameter{std::string{"two"}}},
         {Parameter{std::int64_t{3}}, Parameter{std::string{"three"}}}});
    EXPECT_EQ(affected, (std::vector<std::int64_t>{1, 1, 1}));

    auto count = conn->query("SELECT COUNT(*) FROM test_pg_batch_values");
    ASSERT_TRUE(count->next());
    EXPECT_EQ(count->get_int(0), 3);

    conn->begin_transaction();
    EXPECT_THROW(
        conn->execute_batch(
            "INSERT INTO test_pg_batch_values (id, value) VALUES (?, ?)",
            {{Parameter{std::int64_t{4}}, Parameter{std::string{"four"}}},
             {Parameter{std::int64_t{1}}, Parameter{std::string{"duplicate"}}}}),
        UniqueConstraintViolationException);
    conn->rollback();

    auto after_rollback = conn->query("SELECT COUNT(*) FROM test_pg_batch_values");
    ASSERT_TRUE(after_rollback->next());
    EXPECT_EQ(after_rollback->get_int(0), 3);
    conn->execute("DROP TABLE IF EXISTS test_pg_batch_values;");
}

TEST(PostgresSessionStoreTest, PersistsAndExpiresSharedSessions) {
    std::unique_ptr<novaboot::testing::PostgresTestDatabase> database;
    try {
        database = std::make_unique<novaboot::testing::PostgresTestDatabase>();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "PostgreSQL server not available: " << e.what();
    }
    auto datasource = database->datasource();
    auto connection = datasource->get_connection();
    connection->execute(novaboot::db::postgres::PostgresSessionStore::schema_ddl());
    connection.reset();

    novaboot::db::postgres::PostgresSessionStore store(datasource);
    const auto now = std::chrono::system_clock::now();
    store.put({
        .id = "shared-session",
        .principal = {.subject = "alice", .roles = {"admin", "editor"}, .scopes = {"articles:write"}},
        .expires_at = now + std::chrono::hours{1},
    });

    const auto found = store.find("shared-session", now);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->principal.subject, "alice");
    EXPECT_EQ(found->principal.roles, (std::vector<std::string>{"admin", "editor"}));
    EXPECT_EQ(found->principal.scopes, (std::vector<std::string>{"articles:write"}));

    store.erase("shared-session");
    EXPECT_FALSE(store.find("shared-session", now).has_value());

    store.put({
        .id = "expired-session",
        .principal = {.subject = "bob", .roles = {}, .scopes = {}},
        .expires_at = now - std::chrono::hours{1},
    });
    EXPECT_EQ(store.erase_expired(now), 1);
    EXPECT_FALSE(store.find("expired-session", now).has_value());
}

TEST(PostgresTransactionTest, OptimisticLockAcrossTransactions) {
    std::unique_ptr<novaboot::testing::PostgresTestDatabase> database;
    try {
        database = std::make_unique<novaboot::testing::PostgresTestDatabase>();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "PostgreSQL server not available: " << e.what();
    }
    auto ds = database->datasource();

    {
        auto conn = ds->get_connection();
        conn->execute("DROP TABLE IF EXISTS test_postgres_versioned_entities;");
        SchemaGenerator::create_table<VersionedPostgresEntity>(*ds);
    }

    VersionedPostgresRepository outside_transaction(ds);
    auto saved = outside_transaction.save(VersionedPostgresEntity{.title = "original"});
    ASSERT_EQ(saved.version, 1);

    Transaction transaction_a(ds);
    Transaction transaction_b(ds);
    VersionedPostgresRepository repo_a(ds, transaction_a.connection());
    VersionedPostgresRepository repo_b(ds, transaction_b.connection());

    auto first = repo_a.find_by_id(saved.id);
    auto stale = repo_b.find_by_id(saved.id);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(stale.has_value());

    first->title = "transaction A";
    auto updated = repo_a.save(*first);
    EXPECT_EQ(updated.version, 2);
    transaction_a.commit();

    stale->title = "transaction B";
    EXPECT_THROW(repo_b.save(*stale), OptimisticLockException);
    transaction_b.rollback();

    auto current = outside_transaction.find_by_id(saved.id);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->title, "transaction A");
    EXPECT_EQ(current->version, 2);

    auto conn = ds->get_connection();
    conn->execute("DROP TABLE IF EXISTS test_postgres_versioned_entities;");
}
