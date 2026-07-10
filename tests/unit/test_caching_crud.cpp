#include <gtest/gtest.h>
#include "novaboot/data/caching_crud_repository.h"
#include "db_user.h"
#include <unordered_map>

using namespace novaboot::data;

class MockSqlUserRepository : public CrudRepository<DbUser, int> {
public:
    std::unordered_map<int, DbUser> store;
    int find_calls = 0;
    int save_calls = 0;
    int delete_calls = 0;

    std::optional<DbUser> find_by_id(const int& id) override {
        find_calls++;
        auto it = store.find(id);
        if (it != store.end()) return it->second;
        return std::nullopt;
    }

    std::vector<DbUser> find_all() override {
        std::vector<DbUser> res;
        for (auto& pair : store) res.push_back(pair.second);
        return res;
    }

    DbUser save(const DbUser& e) override {
        save_calls++;
        store[e.id] = e;
        return e;
    }

    void delete_by_id(const int& id) override {
        delete_calls++;
        store.erase(id);
    }

    bool exists_by_id(const int& id) override {
        return store.find(id) != store.end();
    }

    std::size_t count() override {
        return store.size();
    }
};

class MockCacheUserRepository : public CacheRepository<DbUser, int> {
public:
    std::unordered_map<int, DbUser> cache;
    int get_calls = 0;
    int put_calls = 0;
    int evict_calls = 0;

    std::optional<DbUser> get(const int& id) override {
        get_calls++;
        auto it = cache.find(id);
        if (it != cache.end()) return it->second;
        return std::nullopt;
    }

    void put(const int& id, const DbUser& e, std::chrono::seconds) override {
        put_calls++;
        cache[id] = e;
    }

    void evict(const int& id) override {
        evict_calls++;
        cache.erase(id);
    }

    bool exists(const int& id) override {
        return cache.find(id) != cache.end();
    }
};

TEST(CachingCrudRepositoryTest, CacheAsideLifecycle) {
    MockSqlUserRepository sql_repo;
    MockCacheUserRepository cache_repo;
    CachingCrudRepository<DbUser, int> smart_repo(sql_repo, cache_repo, std::chrono::seconds(60));

    // Save a user
    DbUser u;
    u.id = 100;
    u.name = "Alice Smart";
    u.email = "alice@smart.com";
    
    smart_repo.save(u);
    EXPECT_EQ(sql_repo.save_calls, 1);
    EXPECT_EQ(cache_repo.evict_calls, 1); // Evict called on save

    // 1st Read: Cache Miss, SQL Hit, writes to cache
    auto r1 = smart_repo.find_by_id(100);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->name, "Alice Smart");
    EXPECT_EQ(cache_repo.get_calls, 1);
    EXPECT_EQ(sql_repo.find_calls, 1);
    EXPECT_EQ(cache_repo.put_calls, 1);

    // 2nd Read: Cache Hit, no SQL query
    auto r2 = smart_repo.find_by_id(100);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->name, "Alice Smart");
    EXPECT_EQ(cache_repo.get_calls, 2);
    EXPECT_EQ(sql_repo.find_calls, 1); // SQL find_calls remains 1
    EXPECT_EQ(cache_repo.put_calls, 1); // Cache put_calls remains 1

    // Update entity
    u.name = "Alice Updated";
    smart_repo.save(u);
    EXPECT_EQ(sql_repo.save_calls, 2);
    EXPECT_EQ(cache_repo.evict_calls, 2); // Evict called again

    // 3rd Read: Cache Miss again (evicted), SQL Hit, writes to cache
    auto r3 = smart_repo.find_by_id(100);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->name, "Alice Updated");
    EXPECT_EQ(cache_repo.get_calls, 3);
    EXPECT_EQ(sql_repo.find_calls, 2); // SQL query performed
    EXPECT_EQ(cache_repo.put_calls, 2); // Written back to cache
}
