#include "novaboot/observability/http_observation_middleware.h"

#include <chrono>

#include "novaboot/observability/trace_context.h"

namespace novaboot::observability {

HttpObservationMiddleware::HttpObservationMiddleware(std::shared_ptr<MeterRegistry> meters)
    : meters_(std::move(meters)) {}

void HttpObservationMiddleware::handle(http3::Request& request,
                                       http3::Response& response,
                                       context::RequestContext& context,
                                       Next next) {
    const auto started = std::chrono::steady_clock::now();
    inject_trace_context(begin_server_span(request), response, context);
    meters_->up_down_counter_add("http.server.active_requests", 1.0);

    try {
        next();
    } catch (...) {
        meters_->record_span({
            .name = "http.server.request",
            .attributes = {{"http.request.method", std::string(request.method())},
                           {"url.scheme", std::string(request.scheme())},
                           {"trace_id", std::string(context.get_string("trace_id"))}},
            .duration = std::chrono::steady_clock::now() - started,
            .error = true,
        });
        meters_->up_down_counter_add("http.server.active_requests", -1.0);
        throw;
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    Attributes attributes{
        {"http.request.method", std::string(request.method())},
        {"http.response.status_code", std::to_string(response.status_code())},
        {"url.scheme", std::string(request.scheme())},
    };
    meters_->histogram_record("http.server.request.duration", elapsed, "s", attributes);
    meters_->histogram_record("http.server.request.body.size",
                              static_cast<double>(request.content_length()), "By", attributes);
    meters_->histogram_record("http.server.response.body.size",
                              static_cast<double>(response.body_size()), "By", attributes);
    attributes.insert_or_assign("trace_id", std::string(context.get_string("trace_id")));
    meters_->record_span({.name = "http.server.request", .attributes = std::move(attributes),
                          .duration = std::chrono::steady_clock::now() - started,
                          .error = response.status_code() >= 500});
    meters_->up_down_counter_add("http.server.active_requests", -1.0);
}

} // namespace novaboot::observability
