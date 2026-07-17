#include <gtest/gtest.h>
#include "novaboot/db/drivers/postgres/postgres_driver.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/schema.h"
#include "novaboot/db/transaction.h"
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
    std::string conn_info = "host=localhost dbname=postgres user=postgres password=postgres connect_timeout=2";
    
    std::shared_ptr<PostgresDataSource> ds;
    try {
        ds = std::make_shared<PostgresDataSource>(conn_info, 2);
    } catch (const std::exception& e) {
        std::cout << "[WARNING] PostgreSQL not available: " << e.what() << "\n";
        std::cout << "[INFO] Skipping PostgreSQL CRUD integration tests.\n";
        GTEST_SKIP() << "PostgreSQL server not available.";
        return;
    }
    
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
    std::string conn_info = "host=localhost dbname=postgres user=postgres password=postgres connect_timeout=2";

    std::shared_ptr<PostgresDataSource> ds;
    try {
        ds = std::make_shared<PostgresDataSource>(conn_info, 1);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "PostgreSQL server not available: " << e.what();
    }

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

TEST(PostgresTransactionTest, OptimisticLockAcrossTransactions) {
    std::string conn_info = "host=localhost dbname=postgres user=postgres password=postgres connect_timeout=2";
    std::shared_ptr<PostgresDataSource> ds;
    try {
        ds = std::make_shared<PostgresDataSource>(conn_info, 3);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "PostgreSQL server not available: " << e.what();
    }

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
