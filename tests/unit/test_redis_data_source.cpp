#include <gtest/gtest.h>
#include "novaboot/data/redis/redis_data_source.h"

using namespace novaboot::data;
using namespace novaboot::config;

TEST(RedisDataSourceTest, InitSingleMode) {
    RedisConfig cfg;
    cfg.mode = RedisMode::Single;
    cfg.nodes = {"127.0.0.1:6379"};
    cfg.password = "";
    cfg.pool_size = 5;

    EXPECT_NO_THROW({
        RedisDataSource ds(cfg);
        EXPECT_FALSE(ds.is_cluster());
        auto& client = ds.client();
        (void)client;
    });
}

TEST(RedisDataSourceTest, InitClusterMode) {
    RedisConfig cfg;
    cfg.mode = RedisMode::Cluster;
    cfg.nodes = {"127.0.0.1:7000", "127.0.0.1:7001"};
    cfg.password = "";
    cfg.pool_size = 5;

    // Cluster mode attempts to retrieve slot map from nodes on construction, which might fail
    // if Redis is not running. So we expect it could throw a sw::redis::Error or proceed lazily.
    // We catch and check either behavior.
    try {
        RedisDataSource ds(cfg);
        EXPECT_TRUE(ds.is_cluster());
    } catch (const sw::redis::Error& err) {
        // Expected network/connection failure if cluster is down
        SUCCEED() << "Caught expected Redis cluster connection error: " << err.what();
    } catch (...) {
        FAIL() << "Unexpected exception thrown";
    }
}
