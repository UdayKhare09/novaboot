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

/// Start a client span for an outbound request. When `parent` is supplied,
/// the new span stays in that trace and records the supplied span as its
/// parent; otherwise this starts a fresh trace.
[[nodiscard]] TraceContext
begin_client_span(const std::optional<TraceContext>& parent = std::nullopt);

/// Add W3C propagation headers to an outbound request. Existing propagation
/// headers are replaced so a request cannot carry conflicting contexts.
void inject_trace_context(const TraceContext& trace, http3::HeaderMap& headers);

/// Inject the response propagation headers and make the values discoverable to
/// downstream route handlers through RequestContext string keys.
void inject_trace_context(const TraceContext& trace, http3::Response& response,
                          context::RequestContext& context);

} // namespace novaboot::observability
