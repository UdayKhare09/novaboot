#include <gtest/gtest.h>

#include "novaboot/actuator/actuator.h"
#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/router/router.h"

#include <spdlog/spdlog.h>

namespace {

void invoke(novaboot::router::Router& router, std::string_view method, std::string_view path,
            novaboot::http3::Request& request, novaboot::http3::Response& response) {
    const auto match = router.match(method, path);
    ASSERT_NE(match.handler, nullptr);
    request.set_method(method);
    request.set_path(path);
    request.path_params() = match.params;
    novaboot::context::RequestContext context;
    (*match.handler)(request, response, context);
}

void get(novaboot::router::Router& router, std::string_view path,
         novaboot::http3::Request& request, novaboot::http3::Response& response) {
    invoke(router, "GET", path, request, response);
}

TEST(ActuatorTest, ExposesHealthInfoAndMetricsWithExplicitAvailability) {
    novaboot::actuator::Config config;
    config.expose_metrics = true;
    config.expose_prometheus = true;
    config.expose_config = true;
    config.expose_loggers = true;
    config.expose_observations = true;
    novaboot::actuator::Actuator actuator(config);
    actuator.add_info("application", "sample-service");
    actuator.add_health_contributor("database", [] {
        return novaboot::actuator::Health{novaboot::actuator::HealthStatus::Up,
                                          {{"database", "postgres"}}};
    });
    actuator.add_config_source("application.toml", {
        {"server.port", "4435"},
        {"database.password", "never-return-this"},
        {"jwt.signing_key", "never-return-this-either"},
    });
    actuator.meters()->counter_add("custom.work.completed", 2.0, {{"worker", "fast"}});
    actuator.meters()->record_span({.name = "sample.work", .attributes = {}});

    novaboot::router::Router router;
    actuator.register_routes(router);

    novaboot::http3::Request request;
    novaboot::http3::Response response;
    get(router, "/actuator/health/readiness", request, response);
    EXPECT_EQ(response.status_code(), 503);
    EXPECT_NE(response.body_str().find("OUT_OF_SERVICE"), std::string::npos);

    actuator.set_availability(novaboot::actuator::Availability::Ready);
    response = {};
    get(router, "/actuator/health", request, response);
    EXPECT_EQ(response.status_code(), 200);
    EXPECT_NE(response.body_str().find("database"), std::string::npos);
    EXPECT_NE(response.body_str().find("UP"), std::string::npos);

    response = {};
    get(router, "/actuator/info", request, response);
    EXPECT_NE(response.body_str().find("sample-service"), std::string::npos);

    response = {};
    get(router, "/actuator/metrics", request, response);
    EXPECT_NE(response.body_str().find("custom.work.completed"), std::string::npos);

    response = {};
    get(router, "/actuator/prometheus", request, response);
    EXPECT_EQ(response.headers().get("content-type"), "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_NE(response.body_str().find("# TYPE custom_work_completed_total counter"), std::string::npos);
    EXPECT_NE(response.body_str().find("custom_work_completed_total{worker=\"fast\"} 2"), std::string::npos);

    response = {};
    get(router, "/actuator/configprops", request, response);
    EXPECT_NE(response.body_str().find("application.toml"), std::string::npos);
    EXPECT_NE(response.body_str().find("server.port"), std::string::npos);
    EXPECT_NE(response.body_str().find("******"), std::string::npos);
    EXPECT_EQ(response.body_str().find("never-return-this"), std::string::npos);

    response = {};
    get(router, "/actuator/observations", request, response);
    EXPECT_NE(response.body_str().find("sample.work"), std::string::npos);
}

TEST(ActuatorTest, RequiresAnExplicitAuthorizerWhenConfigured) {
    novaboot::actuator::Config config;
    config.require_authorization = true;
    config.authorizer = [](const novaboot::http3::Request& request) {
        return request.header("x-management-token") == "allowed";
    };
    novaboot::actuator::Actuator actuator(config);
    novaboot::router::Router router;
    actuator.register_routes(router);

    novaboot::http3::Request request;
    novaboot::http3::Response response;
    get(router, "/actuator/health", request, response);
    EXPECT_EQ(response.status_code(), 403);

    response = {};
    request.headers().set("x-management-token", "allowed");
    get(router, "/actuator/health", request, response);
    EXPECT_EQ(response.status_code(), 200);
}

TEST(ActuatorTest, ChangesRootLogLevelOnlyWhenTheEndpointIsExplicitlyExposed) {
    novaboot::actuator::Config config;
    config.expose_loggers = true;
    config.require_authorization = true;
    config.authorizer = [](const novaboot::http3::Request& request) {
        return request.header("x-management-token") == "allowed";
    };
    novaboot::actuator::Actuator actuator(config);
    novaboot::router::Router router;
    actuator.register_routes(router);
    const auto original = spdlog::default_logger()->level();

    novaboot::http3::Request request;
    const std::string body = "{\"configuredLevel\":\"debug\"}";
    request.append_body(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    novaboot::http3::Response response;
    invoke(router, "POST", "/actuator/loggers/ROOT", request, response);
    EXPECT_EQ(response.status_code(), 403);
    EXPECT_EQ(spdlog::default_logger()->level(), original);

    response = {};
    request.headers().set("x-management-token", "allowed");
    invoke(router, "POST", "/actuator/loggers/ROOT", request, response);
    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(spdlog::default_logger()->level(), spdlog::level::debug);
    spdlog::set_level(original);
}

} // namespace
