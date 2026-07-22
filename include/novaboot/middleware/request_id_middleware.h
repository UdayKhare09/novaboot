#pragma once

#include <string>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Adds a stable request correlation id to RequestContext and the response.
///
/// A valid inbound header can be trusted for cross-service correlation; invalid
/// values are replaced so control characters never reach response headers/logs.
class RequestIdMiddleware final : public Middleware {
public:
    struct Config {
        std::string header_name = "x-request-id";
        bool trust_inbound_header = true;
        bool add_response_header = true;
    };

    RequestIdMiddleware();
    explicit RequestIdMiddleware(Config config);

    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;

private:
    static bool valid_id(std::string_view value);
    static std::string generate_id();

    Config config_;
};

} // namespace novaboot::middleware
