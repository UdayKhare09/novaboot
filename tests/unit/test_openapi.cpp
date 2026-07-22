#include <gtest/gtest.h>

#include "novaboot/openapi/openapi.h"

namespace {

struct CreateArticle {
    std::string title;
    int priority = 0;
    std::vector<std::string> tags;

    inline static const novaboot::validation::Schema<CreateArticle> validator =
        novaboot::validation::Schema<CreateArticle>()
            .field<&CreateArticle::title>("title").not_empty().size(3, 120)
            .field<&CreateArticle::priority>("priority").min(1).max(5)
            .field<&CreateArticle::tags>("tags").size(0, 10);
};

} // namespace

TEST(OpenApiDocumentTest, DescribesRegisteredRoutesAndPathParameters) {
    novaboot::router::Router router;
    router.route("/articles/:article_id").get([](auto&, auto&, auto&) {});
    router.route("/articles").post([](auto&, auto&, auto&) {});

    novaboot::openapi::Document document(router, {
        .title = "Knowledge Hub",
        .version = "1.2.3",
        .servers = {"https://api.example.test"},
    });
    const auto json = document.json();

    EXPECT_NE(json.find(R"("openapi":"3.1.0")"), std::string::npos);
    EXPECT_NE(json.find(R"("/articles/{article_id}")"), std::string::npos);
    EXPECT_NE(json.find(R"("operationId":"get_articles__article_id")"), std::string::npos);
    EXPECT_NE(json.find(R"("name":"article_id","in":"path","required":true)"), std::string::npos);
    EXPECT_NE(json.find(R"("post")"), std::string::npos);
}

TEST(OpenApiDocumentTest, AddsAJsonEndpointSnapshot) {
    novaboot::router::Router router;
    router.route("/health").get([](auto&, auto&, auto&) {});
    novaboot::openapi::serve(router, "/docs/openapi.json");

    const auto match = router.match("GET", "/docs/openapi.json");
    ASSERT_NE(match.handler, nullptr);
    novaboot::http3::Request request;
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    (*match.handler)(request, response, context);

    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(response.headers().get("content-type").value_or(""),
              "application/vnd.oai.openapi+json;version=3.1");
    EXPECT_NE(response.body_str().find(R"("/health")"), std::string::npos);
}

TEST(OpenApiDocumentTest, EmitsValidationSchemaAndOperationRequestBody) {
    novaboot::router::Router router;
    router.route("/articles").post([](auto&, auto&, auto&) {});

    novaboot::openapi::Document document(router);
    document.schema("CreateArticle", CreateArticle::validator)
        .request_body("/articles", novaboot::router::Method::POST, "CreateArticle");
    const auto json = document.json();
    SCOPED_TRACE(json);

    EXPECT_NE(json.find(R"("components":{"schemas":{"CreateArticle":{)"), std::string::npos);
    EXPECT_NE(json.find(R"("title":{"type":"string","minLength":3,"maxLength":120})"),
              std::string::npos);
    EXPECT_NE(json.find(R"("priority":{"type":"integer","minimum":1,"maximum":5})"),
              std::string::npos);
    EXPECT_NE(json.find(R"("tags":{"type":"array","items":{"type":"string"})"),
              std::string::npos);
    EXPECT_NE(json.find(R"("required":["title"])"), std::string::npos);
    EXPECT_NE(json.find(R"("requestBody":{"required":true,"content":{"application/json":{"schema":{"$ref":"#/components/schemas/CreateArticle"})"),
              std::string::npos);
}
