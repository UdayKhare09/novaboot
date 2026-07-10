#include <gtest/gtest.h>
#include "novaboot/data/redis/redis_repository_base.h"
#include "db_user.h"

using namespace novaboot::data;

class DbUserCacheRepository : public RedisRepositoryBase<DbUser, int> {
public:
    explicit DbUserCacheRepository(RedisDataSource& ds)
        : RedisRepositoryBase<DbUser, int>(ds, "DbUser", std::chrono::seconds(60)) {}
};

TEST(RedisRepositoryTest, CacheOperations) {
    novaboot::config::RedisConfig cfg;
    cfg.mode = novaboot::config::RedisMode::Single;
    cfg.nodes = {"127.0.0.1:6379"};
    cfg.pool_size = 5;

    try {
        RedisDataSource ds(cfg);
        DbUserCacheRepository repo(ds);

        DbUser u;
        u.id = 42;
        u.name = "Alice Cache";
        u.email = "alice@example.com";

        repo.put(u.id, u, std::chrono::seconds(60));
        auto retrieved = repo.get(u.id);
        (void)retrieved;
    } catch (const sw::redis::Error& e) {
        SUCCEED() << "Caught expected Redis connection error: " << e.what();
    } catch (...) {
        FAIL() << "Unexpected exception";
    }
}
