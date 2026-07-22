#include <gtest/gtest.h>

#include "novaboot/openapi/openapi.h"

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
