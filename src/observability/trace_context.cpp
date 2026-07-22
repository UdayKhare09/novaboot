#include "novaboot/observability/trace_context.h"

#include <cctype>
#include <stdexcept>
#include <vector>

#include <openssl/rand.h>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::observability {
namespace {

bool is_lower_hex(std::string_view value) {
    if (value.empty()) return false;
    for (const unsigned char c : value) {
        if (!(std::isdigit(c) || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool all_zero(std::string_view value) {
    for (const char c : value) if (c != '0') return false;
    return true;
}

std::string random_hex(std::size_t byte_count) {
    static constexpr char hex[] = "0123456789abcdef";
    std::vector<unsigned char> bytes(byte_count);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("OpenSSL could not generate a trace identifier");
    }
    std::string result;
    result.reserve(byte_count * 2);
    for (const auto value : bytes) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0f]);
    }
    return result;
}

} // namespace

std::string TraceContext::traceparent() const {
    return "00-" + trace_id + "-" + span_id + "-" + trace_flags;
}

std::optional<TraceContext> extract_trace_context(const http3::Request& request) {
    const auto header = request.header("traceparent");
    if (!header || header->size() != 55) return std::nullopt;
    const std::string_view value = *header;
    if (value[2] != '-' || value[35] != '-' || value[52] != '-') return std::nullopt;
    const auto version = value.substr(0, 2);
    const auto trace_id = value.substr(3, 32);
    const auto parent_id = value.substr(36, 16);
    const auto flags = value.substr(53, 2);
    if (version == "ff" || !is_lower_hex(version) || !is_lower_hex(trace_id) ||
        !is_lower_hex(parent_id) || !is_lower_hex(flags) || all_zero(trace_id) ||
        all_zero(parent_id)) return std::nullopt;

    TraceContext context;
    context.trace_id = trace_id;
    context.parent_span_id = parent_id;
    context.trace_flags = flags;
    if (const auto state = request.header("tracestate")) context.tracestate = *state;
    return context;
}

TraceContext begin_server_span(const http3::Request& request) {
    TraceContext context;
    if (auto extracted = extract_trace_context(request)) context = std::move(*extracted);
    if (context.trace_id.empty()) context.trace_id = random_hex(16);
    if (context.trace_flags.empty()) context.trace_flags = "01";
    context.span_id = random_hex(8);
    return context;
}

void inject_trace_context(const TraceContext& trace, http3::Response& response,
                          context::RequestContext& context) {
    response.header("traceparent", trace.traceparent());
    if (!trace.tracestate.empty()) response.header("tracestate", trace.tracestate);
    context.set<TraceContext>(trace);
    context.set_string("trace_id", trace.trace_id);
    context.set_string("span_id", trace.span_id);
    context.set_string("parent_span_id", trace.parent_span_id);
}

} // namespace novaboot::observability
