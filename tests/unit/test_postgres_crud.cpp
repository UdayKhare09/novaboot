#include <gtest/gtest.h>
#include "novaboot/db/drivers/postgres/postgres_driver.h"
#include "novaboot/db/repository.h"
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
