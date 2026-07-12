#pragma once

#include <string>
#include <unordered_map>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Adds common defensive HTTP response headers.
class SecurityHeadersMiddleware : public Middleware {
public:
    struct Config {
        bool hsts = true;
        std::string hsts_value = "max-age=31536000; includeSubDomains";

        bool content_type_options = true;
        std::string content_type_options_value = "nosniff";

        bool frame_options = true;
        std::string frame_options_value = "DENY";

        bool referrer_policy = true;
        std::string referrer_policy_value = "no-referrer";

        bool permissions_policy = true;
        std::string permissions_policy_value =
            "camera=(), microphone=(), geolocation=()";

        bool cross_origin_opener_policy = true;
        std::string cross_origin_opener_policy_value = "same-origin";

        bool cross_origin_resource_policy = true;
        std::string cross_origin_resource_policy_value = "same-origin";

        std::string content_security_policy;
        std::unordered_map<std::string, std::string> custom_headers = {};
    };

    SecurityHeadersMiddleware();
    explicit SecurityHeadersMiddleware(Config cfg);

    void handle(http3::Request& req,
                http3::Response& res,
                context::RequestContext& ctx,
                Next next) override;

private:
    Config cfg_;
};

} // namespace novaboot::middleware
