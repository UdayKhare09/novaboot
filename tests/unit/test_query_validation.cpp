#include <gtest/gtest.h>
#include "novaboot/core/server.h"
#include <string>
#include <vector>

using namespace novaboot;

struct TestQuery {
    int page = 1;
    int limit = 10;
    std::string q;

    inline static const novaboot::validation::Schema<TestQuery> validator =
        novaboot::validation::Schema<TestQuery>()
            .field<&TestQuery::page>("page").min(1)
            .field<&TestQuery::limit>("limit").min(5).max(100)
            .field<&TestQuery::q>("q").not_empty();
};

class TestController {
public:
    int last_id = 0;
    std::string last_q;
    int last_page = 0;
    int last_limit = 0;
    bool called = false;

    void get_item(int id, TestQuery query) {
        called = true;
        last_id = id;
        last_q = query.q;
        last_page = query.page;
        last_limit = query.limit;
    }
};

#ifdef __cpp_impl_reflection
TEST(QueryValidationTest, BindAndValidateQueryStruct) {
    TestController controller;
    http3::Request req;
    req.set_method("GET");
    req.set_path("/api/items/42?page=3&limit=20&q=test_search");
    req.path_params().set("id", "42");

    http3::Response res;
    context::RequestContext ctx;

    // Use Invoker to simulate request resolution
    using InvokerType = detail::Invoker<
        TestController,
        ^^TestController::get_item,
        void,
        int,
        TestQuery
    >;

    InvokerType::invoke(controller, &TestController::get_item, req, res, ctx);

    EXPECT_TRUE(controller.called);
    EXPECT_EQ(controller.last_id, 42);
    EXPECT_EQ(controller.last_page, 3);
    EXPECT_EQ(controller.last_limit, 20);
    EXPECT_EQ(controller.last_q, "test_search");
}

TEST(QueryValidationTest, BindAndValidateQueryStructFailsValidation) {
    TestController controller;
    http3::Request req;
    req.set_method("GET");
    // Limit is 2 (min is 5), page is 0 (min is 1)
    req.set_path("/api/items/42?page=0&limit=2&q=");
    req.path_params().set("id", "42");

    http3::Response res;
    context::RequestContext ctx;

    using InvokerType = detail::Invoker<
        TestController,
        ^^TestController::get_item,
        void,
        int,
        TestQuery
    >;

    EXPECT_THROW(
        InvokerType::invoke(controller, &TestController::get_item, req, res, ctx),
        novaboot::validation::ValidationException
    );
}

struct ZeroCopyDto {
    std::string_view name;
    int age = 0;

    inline static const novaboot::validation::Schema<ZeroCopyDto> validator =
        novaboot::validation::Schema<ZeroCopyDto>()
            .field<&ZeroCopyDto::name>("name").not_empty()
            .field<&ZeroCopyDto::age>("age").min(18);
};

class ZeroCopyController {
public:
    std::string_view last_name;
    int last_age = 0;
    bool called = false;

    void post_user(ZeroCopyDto dto) {
        called = true;
        last_name = dto.name;
        last_age = dto.age;
    }
};

TEST(QueryValidationTest, ZeroCopyJsonDeserialization) {
    ZeroCopyController controller;
    http3::Request req;
    req.set_method("POST");
    const char* payload = "{\"name\":\"John Doe\",\"age\":30}";
    req.append_body(reinterpret_cast<const uint8_t*>(payload), strlen(payload));

    http3::Response res;
    context::RequestContext ctx;

    using InvokerType = detail::Invoker<
        ZeroCopyController,
        ^^ZeroCopyController::post_user,
        void,
        ZeroCopyDto
    >;

    try {
        InvokerType::invoke(controller, &ZeroCopyController::post_user, req, res, ctx);
    } catch (const novaboot::validation::ValidationException& ex) {
        for (const auto& err : ex.errors()) {
            std::cout << "Validation Error: " << err << std::endl;
        }
        throw;
    }

    EXPECT_TRUE(controller.called);
    EXPECT_EQ(controller.last_name, "John Doe");
    EXPECT_EQ(controller.last_age, 30);
}

struct NestedSettingsDto {
    bool enabled = false;
    std::vector<int> levels;
};

struct RichBodyDto {
    std::string title;
    NestedSettingsDto settings;
    std::vector<std::string> tags;

    inline static const novaboot::validation::Schema<RichBodyDto> validator =
        novaboot::validation::Schema<RichBodyDto>()
            .field<&RichBodyDto::title>("title").not_empty();
};

class RichBodyController {
public:
    RichBodyDto last;
    bool called = false;

    void post_rich(RichBodyDto dto) {
        called = true;
        last = std::move(dto);
    }
};

TEST(QueryValidationTest, JsonBodyStructMayContainNestedObjectsAndVectors) {
    RichBodyController controller;
    http3::Request req;
    req.set_method("POST");
    const char* payload = R"({
        "title": "nested",
        "settings": { "enabled": true, "levels": [1, 2, 3] },
        "tags": ["orm", "json"]
    })";
    req.append_body(reinterpret_cast<const uint8_t*>(payload), strlen(payload));

    http3::Response res;
    context::RequestContext ctx;

    using InvokerType = detail::Invoker<
        RichBodyController,
        ^^RichBodyController::post_rich,
        void,
        RichBodyDto
    >;

    InvokerType::invoke(controller, &RichBodyController::post_rich, req, res, ctx);

    ASSERT_TRUE(controller.called);
    EXPECT_EQ(controller.last.title, "nested");
    EXPECT_TRUE(controller.last.settings.enabled);
    ASSERT_EQ(controller.last.settings.levels.size(), 3);
    EXPECT_EQ(controller.last.settings.levels[2], 3);
    ASSERT_EQ(controller.last.tags.size(), 2);
    EXPECT_EQ(controller.last.tags[1], "json");
}

TEST(QueryValidationTest, MalformedJsonBodyThrowsBadRequestError) {
    RichBodyController controller;
    http3::Request req;
    req.set_method("POST");
    const char* payload = R"({"title": "broken")";
    req.append_body(reinterpret_cast<const uint8_t*>(payload), strlen(payload));

    http3::Response res;
    context::RequestContext ctx;

    using InvokerType = detail::Invoker<
        RichBodyController,
        ^^RichBodyController::post_rich,
        void,
        RichBodyDto
    >;

    EXPECT_THROW(
        InvokerType::invoke(controller, &RichBodyController::post_rich, req, res, ctx),
        std::invalid_argument);
    EXPECT_FALSE(controller.called);
}

TEST(QueryValidationTest, JsonBindingErrorsNameTheNestedFieldPath) {
    RichBodyController controller;
    http3::Request req;
    req.set_method("POST");
    const char* payload = R"({
        "title": "nested",
        "settings": { "enabled": true, "levels": [1, "wrong"] },
        "tags": ["json"]
    })";
    req.append_body(reinterpret_cast<const uint8_t*>(payload), strlen(payload));

    http3::Response res;
    context::RequestContext ctx;
    using InvokerType = detail::Invoker<
        RichBodyController,
        ^^RichBodyController::post_rich,
        void,
        RichBodyDto
    >;

    try {
        InvokerType::invoke(controller, &RichBodyController::post_rich, req, res, ctx);
        FAIL() << "Expected JSON binding failure";
    } catch (const novaboot::json::BindingException& error) {
        ASSERT_EQ(error.errors().size(), 1U);
        EXPECT_EQ(error.errors().front(), "$.settings.levels[1]: expected integer");
    }
    EXPECT_FALSE(controller.called);
}
#endif
