#include <gtest/gtest.h>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/observability/http_observation_middleware.h"
#include "novaboot/observability/trace_context.h"

namespace {

TEST(TraceContextTest, ContinuesValidW3CTraceAndInjectsResponseHeader) {
    novaboot::http3::Request request;
    request.headers().set("traceparent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    request.headers().set("tracestate", "vendor=value");

    const auto trace = novaboot::observability::begin_server_span(request);
    EXPECT_EQ(trace.trace_id, "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT_EQ(trace.parent_span_id, "00f067aa0ba902b7");
    EXPECT_EQ(trace.trace_flags, "01");
    EXPECT_EQ(trace.span_id.size(), 16U);
    EXPECT_NE(trace.span_id, trace.parent_span_id);

    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    novaboot::observability::inject_trace_context(trace, response, context);
    EXPECT_EQ(response.headers().get("traceparent"), trace.traceparent());
    EXPECT_EQ(response.headers().get("tracestate"), "vendor=value");
    EXPECT_EQ(context.get_string("trace_id"), trace.trace_id);
}

TEST(TraceContextTest, RejectsMalformedTraceparentAndStartsNewTrace) {
    novaboot::http3::Request request;
    request.headers().set("traceparent", "00-00000000000000000000000000000000-0000000000000000-01");

    EXPECT_FALSE(novaboot::observability::extract_trace_context(request));
    const auto trace = novaboot::observability::begin_server_span(request);
    EXPECT_EQ(trace.trace_id.size(), 32U);
    EXPECT_EQ(trace.span_id.size(), 16U);
}

TEST(HttpObservationMiddlewareTest, RecordsOtelHttpMetricsWithoutRawPathLabels) {
    auto meters = std::make_shared<novaboot::observability::MeterRegistry>();
    novaboot::middleware::Pipeline pipeline;
    pipeline.add(std::make_shared<novaboot::observability::HttpObservationMiddleware>(meters));

    novaboot::http3::Request request;
    request.set_method("POST");
    request.set_scheme("https");
    request.set_path("/users/42?include=private");
    const std::string body = "request";
    request.append_body(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    novaboot::router::Handler handler = [](auto&, auto& route_response, auto&) {
        route_response.status(201).body("created");
    };

    pipeline.execute(request, response, context, handler);

    EXPECT_TRUE(response.headers().has("traceparent"));
    ASSERT_EQ(meters->spans().size(), 1U);
    EXPECT_EQ(meters->spans()[0].name, "http.server.request");
    const auto metrics = meters->snapshot();
    ASSERT_EQ(metrics.size(), 4U);
    bool duration_found = false;
    for (const auto& metric : metrics) {
        EXPECT_FALSE(metric.attributes.contains("url.path"));
        if (metric.name == "http.server.request.duration") {
            duration_found = true;
            EXPECT_EQ(metric.unit, "s");
            EXPECT_EQ(metric.count, 1U);
            EXPECT_EQ(metric.attributes.at("http.request.method"), "POST");
            EXPECT_EQ(metric.attributes.at("http.response.status_code"), "201");
        }
    }
    EXPECT_TRUE(duration_found);
}

TEST(ObservationRegistryTest, RetainsCompletedSpansAndStructuredEvents) {
    novaboot::observability::MeterRegistry registry;
    registry.record_span({.name = "order.reserve", .attributes = {{"service.name", "orders"}},
                          .duration = std::chrono::milliseconds(4), .error = false});
    registry.record_event({.name = "order.reserved", .attributes = {{"order.type", "standard"}}});
    ASSERT_EQ(registry.spans().size(), 1U);
    EXPECT_EQ(registry.spans()[0].name, "order.reserve");
    ASSERT_EQ(registry.events().size(), 1U);
    EXPECT_EQ(registry.events()[0].name, "order.reserved");
}

} // namespace
