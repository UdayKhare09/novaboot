#include <gtest/gtest.h>
#include "novaboot/config/app_config.h"
#include <fstream>
#include <filesystem>

using namespace novaboot::config;
namespace fs = std::filesystem;

class AppConfigTest : public ::testing::Test {
protected:
    std::string test_toml_path = "test_application.toml";

    void SetUp() override {
        std::ofstream out(test_toml_path);
        out << R"(
[server]
host = "127.0.0.1"
port = 8080
workers = 4
tls.cert = "test_cert.pem"
tls.key = "test_key.pem"
static_resources = "test_static"

[datasource.postgres]
host     = "db.local"
port     = 5433
user     = "test_user"
password = "test_password"
database = "test_db"
pool.min = 5
pool.max = 15

[datasource.redis]
mode     = "cluster"
nodes    = ["10.0.0.1:6379", "10.0.0.2:6379"]
password = "redis_pass"
pool.size = 12
pool.timeout_ms = 1000
read_from = "master"
cluster.slot_refresh_interval_ms = 5000

[custom]
api_key = "secret_key"
timeout = 30.5
retries = 3
enabled = true
)";
    }

    void TearDown() override {
        fs::remove(test_toml_path);
    }
};

TEST_F(AppConfigTest, LoadValidConfig) {
    AppConfig cfg = AppConfig::load(test_toml_path);

    // Verify [server]
    EXPECT_EQ(cfg.server().host, "127.0.0.1");
    EXPECT_EQ(cfg.server().port, 8080);
    EXPECT_EQ(cfg.server().workers, 4);
    EXPECT_EQ(cfg.server().tls_cert, "test_cert.pem");
    EXPECT_EQ(cfg.server().tls_key, "test_key.pem");
    EXPECT_EQ(cfg.server().static_resources, "test_static");

    // Verify [datasource.postgres]
    EXPECT_EQ(cfg.postgres().host, "db.local");
    EXPECT_EQ(cfg.postgres().port, 5433);
    EXPECT_EQ(cfg.postgres().user, "test_user");
    EXPECT_EQ(cfg.postgres().password, "test_password");
    EXPECT_EQ(cfg.postgres().database, "test_db");
    EXPECT_EQ(cfg.postgres().pool_min, 5);
    EXPECT_EQ(cfg.postgres().pool_max, 15);

    // Verify [datasource.redis]
    EXPECT_EQ(cfg.redis().mode, RedisMode::Cluster);
    ASSERT_EQ(cfg.redis().nodes.size(), 2);
    EXPECT_EQ(cfg.redis().nodes[0], "10.0.0.1:6379");
    EXPECT_EQ(cfg.redis().nodes[1], "10.0.0.2:6379");
    EXPECT_EQ(cfg.redis().password, "redis_pass");
    EXPECT_EQ(cfg.redis().pool_size, 12);
    EXPECT_EQ(cfg.redis().pool_timeout_ms, 1000);
    EXPECT_EQ(cfg.redis().read_from, ReadFrom::Master);
    EXPECT_EQ(cfg.redis().slot_refresh_interval_ms, 5000);

    // Verify custom fields using generic getter
    EXPECT_EQ(cfg.get<std::string>("custom.api_key").value_or(""), "secret_key");
    EXPECT_DOUBLE_EQ(cfg.get<double>("custom.timeout").value_or(0.0), 30.5);
    EXPECT_EQ(cfg.get<int>("custom.retries").value_or(0), 3);
    EXPECT_TRUE(cfg.get<bool>("custom.enabled").value_or(false));
}

TEST_F(AppConfigTest, NonExistentFileThrows) {
    EXPECT_THROW(AppConfig::load("non_existent_file.toml"), std::runtime_error);
}
