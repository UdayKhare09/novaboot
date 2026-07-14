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



    // Verify custom fields using generic getter
    EXPECT_EQ(cfg.get<std::string>("custom.api_key").value_or(""), "secret_key");
    EXPECT_DOUBLE_EQ(cfg.get<double>("custom.timeout").value_or(0.0), 30.5);
    EXPECT_EQ(cfg.get<int>("custom.retries").value_or(0), 3);
    EXPECT_TRUE(cfg.get<bool>("custom.enabled").value_or(false));
}

TEST_F(AppConfigTest, NonExistentFileThrows) {
    EXPECT_THROW(AppConfig::load("non_existent_file.toml"), std::runtime_error);
}
