#include "novaboot/middleware/request_id_middleware.h"

#include <stdexcept>

#include <openssl/rand.h>

namespace novaboot::middleware {

RequestIdMiddleware::RequestIdMiddleware() : RequestIdMiddleware(Config{}) {}

RequestIdMiddleware::RequestIdMiddleware(Config config) : config_(std::move(config)) {
    if (config_.header_name.empty()) {
        throw std::invalid_argument("Request ID header name must not be empty");
    }
}

bool RequestIdMiddleware::valid_id(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char character : value) {
        if (character < 0x21 || character > 0x7e) return false;
    }
    return true;
}

std::string RequestIdMiddleware::generate_id() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("OpenSSL could not generate a request identifier");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(sizeof(bytes) * 2);
    for (const auto value : bytes) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0f]);
    }
    return result;
}

void RequestIdMiddleware::handle(http3::Request& request, http3::Response& response,
                                 context::RequestContext& context, Next next) {
    std::string request_id;
    if (config_.trust_inbound_header) {
        if (const auto inbound = request.header(config_.header_name); inbound && valid_id(*inbound)) {
            request_id = *inbound;
        }
    }
    if (request_id.empty()) request_id = generate_id();

    context.set_string("request_id", request_id);
    if (config_.add_response_header) response.header(config_.header_name, request_id);
    next();
}

} // namespace novaboot::middleware
