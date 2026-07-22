#include <gtest/gtest.h>

#include "novaboot/novaboot.h"
#include "novaboot/annotations/scanner.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/testing/app_test_client.h"
#include "novaboot/middleware/request_id_middleware.h"
#include "novaboot/validation/validation.h"

#include <string>
#include <vector>

using namespace novaboot::annotations;

namespace {

struct HarnessPayload {
    std::string title;
    std::vector<std::string> tags;

    inline static const novaboot::validation::Schema<HarnessPayload> validator =
        novaboot::validation::Schema<HarnessPayload>()
            .field<&HarnessPayload::title>("title").not_empty();
};

struct HarnessResponse {
    int id = 0;
    std::string title;
    int tag_count = 0;
};

struct [[= RestController("/harness") ]] HarnessController {
    [[= GetMapping("/items/:id") ]]
    novaboot::ResponseEntity<HarnessResponse> get_item(int id) {
        return novaboot::ResponseEntity<HarnessResponse>::ok(HarnessResponse{
            .id = id,
            .title = "loaded",
            .tag_count = 0,
        });
    }

    [[= PostMapping("/items") ]]
    novaboot::ResponseEntity<HarnessResponse> create_item(HarnessPayload payload) {
        return novaboot::ResponseEntity<HarnessResponse>::status(201, HarnessResponse{
            .id = 7,
            .title = payload.title,
            .tag_count = static_cast<int>(payload.tags.size()),
        });
    }
};

} // namespace

TEST(AppTestClientTest, DispatchesRegisteredRoutesInProcess) {
    novaboot::di::RootContainer root;
    novaboot::annotations::register_beans<HarnessController>(root);
    root.build();

    novaboot::testing::AppTestClient client(root);

    auto get = client.get("/harness/items/42");
    EXPECT_EQ(get.status, 200);
    EXPECT_NE(get.body.find(R"("id":42)"), std::string::npos);

    auto post = client.post("/harness/items", R"({"title":"created","tags":["a","b"]})");
    EXPECT_EQ(post.status, 201);
    EXPECT_NE(post.body.find(R"("title":"created")"), std::string::npos);
    EXPECT_NE(post.body.find(R"("tag_count":2)"), std::string::npos);

    auto missing = client.get("/harness/missing");
    EXPECT_EQ(missing.status, 404);

    root.shutdown();
}

TEST(AppTestClientTest, ExecutesConfiguredMiddlewareInProcess) {
    novaboot::di::RootContainer root;
    novaboot::annotations::register_beans<HarnessController>(root);
    root.build();
    novaboot::testing::AppTestClient client(root, {
        std::make_shared<novaboot::middleware::RequestIdMiddleware>()
    });
    auto response = client.get("/harness/items/1", {{"x-request-id", "edge-123"}});
    EXPECT_EQ(response.header("x-request-id"), "edge-123");
    EXPECT_TRUE(response.body_contains(R"("id":1)"));
    root.shutdown();
}

TEST(AppTestClientTest, ReportsNestedJsonBindingErrorsAsBadRequest) {
    novaboot::di::RootContainer root;
    novaboot::annotations::register_beans<HarnessController>(root);
    root.build();
    novaboot::testing::AppTestClient client(root);

    const auto response = client.post(
        "/harness/items",
        R"({"title":"created","tags":["valid",7]})");

    EXPECT_EQ(response.status, 400);
    EXPECT_TRUE(response.body_contains("Invalid JSON request body"));
    EXPECT_TRUE(response.body_contains("$.tags[1]: expected string"));
    root.shutdown();
}
