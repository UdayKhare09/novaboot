#include <gtest/gtest.h>
#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/db/repository.h"
#include <filesystem>
#include <vector>
#include <string>

using namespace novaboot;
using namespace novaboot::db;
using namespace novaboot::db::sqlite;
using namespace novaboot::annotations;

struct [[= Entity("test_entities") ]] DBTestEntity {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;
    
    std::string name;
    int score = 0;
};

struct TestEntityRepository : public CrudRepository<DBTestEntity, int> {
    explicit TestEntityRepository(std::shared_ptr<DataSource> ds) 
        : CrudRepository<DBTestEntity, int>(ds) {}
        
    std::vector<DBTestEntity> find_by_score_above(int score) {
        return query()
            .where<&DBTestEntity::score>(Op::GreaterThan, score)
            .order_by<&DBTestEntity::score>(false /* desc */)
            .list();
    }
};

TEST(DatabaseCrudTest, LifecycleAndQuery) {
    std::string db_file = "test_crud.db";
    std::filesystem::remove(db_file); // Ensure fresh start
    
    {
        auto ds = std::make_shared<SqliteDataSource>(db_file, 2);
        
        // Create table
        auto conn = ds->get_connection();
        conn->execute(R"(
            CREATE TABLE test_entities (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                score INTEGER NOT NULL
            );
        )");
        
        TestEntityRepository repo(ds);
        
        // 1. Save new entities
        DBTestEntity e1{0, "Bob", 85};
        DBTestEntity e2{0, "Alice", 95};
        
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
        // Alice (95) and Bob (90) should both match, sorted by score desc (Alice then Bob)
        ASSERT_EQ(matches.size(), 2);
        EXPECT_EQ(matches[0].name, "Alice");
        EXPECT_EQ(matches[1].name, "Bob");
        
        // 6. Delete
        repo.delete_by_id(1);
        EXPECT_FALSE(repo.exists_by_id(1));
        EXPECT_TRUE(repo.exists_by_id(2));
    }
    
    std::filesystem::remove(db_file); // Clean up
}
