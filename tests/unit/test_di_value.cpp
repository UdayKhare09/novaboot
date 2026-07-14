#include <gtest/gtest.h>
#include "novaboot/novaboot.h"
#include "novaboot/di/di.h"
#include "novaboot/annotations/scanner.h"
#include <fstream>
#include <filesystem>

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;
using namespace novaboot::config;
namespace fs = std::filesystem;

struct [[= Component() ]] ValueTestComponent {
    [[= Value("custom.api_key") ]]
    std::string string_val = "default_string";

    [[= Value("custom.retries") ]]
    int int_val = 42;

    [[= Value("custom.timeout") ]]
    double double_val = 3.14;

    [[= Value("custom.enabled") ]]
    bool bool_val = false;

    [[= Value("custom.missing") ]]
    int missing_val = 100;
};

static_assert(has_annotation<Component>(^^ValueTestComponent), "ValueTestComponent must have Component annotation");

class DiValueTest : public ::testing::Test {
protected:
    std::string test_toml_path = "test_di_value.toml";

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

TEST_F(DiValueTest, ValueAnnotationInjection) {
    RootContainer di_root;

    // Load and register AppConfig bean
    auto cfg = AppConfig::load(test_toml_path);
    di_root.register_bean<AppConfig>([cfg](ContainerBase&) {
        return new AppConfig(cfg);
    });

    // Scan and register the component
    register_beans<ValueTestComponent>(di_root);

    di_root.build();

    // Verify injected properties
    EXPECT_TRUE(di_root.has<ValueTestComponent>());
    auto& component = di_root.resolve<ValueTestComponent>();

    EXPECT_EQ(component.string_val, "secret_key");
    EXPECT_EQ(component.int_val, 3);
    EXPECT_DOUBLE_EQ(component.double_val, 30.5);
    EXPECT_TRUE(component.bool_val);
    EXPECT_EQ(component.missing_val, 100); // kept default since key was missing

    di_root.shutdown();
}
