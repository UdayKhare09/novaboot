#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::observability {

/// The W3C Trace Context carried by an inbound server request.
struct TraceContext {
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::string trace_flags;
    std::string tracestate;

    [[nodiscard]] std::string traceparent() const;
};

/// Parse an inbound W3C `traceparent` / `tracestate` pair. Malformed headers
/// are ignored rather than breaking the request.
[[nodiscard]] std::optional<TraceContext>
extract_trace_context(const http3::Request& request);

/// Create a server span context. It keeps a valid remote trace id, if one was
/// supplied, and otherwise starts a new trace.
[[nodiscard]] TraceContext begin_server_span(const http3::Request& request);

/// Inject the response propagation headers and make the values discoverable to
/// downstream route handlers through RequestContext string keys.
void inject_trace_context(const TraceContext& trace, http3::Response& response,
                          context::RequestContext& context);

} // namespace novaboot::observability
