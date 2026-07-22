#pragma once

#include "novaboot/middleware/middleware.h"

#include <string>
#include <vector>

namespace novaboot::middleware {

/// Applies RFC 7239 `Forwarded` proto/host values only after an application
/// explicitly declares and verifies trusted reverse-proxy peers. Only an
/// accepted direct peer may replace the effective client address from `for=`.
class TrustedForwardedHeadersMiddleware final : public Middleware {
public:
    struct Config {
        /// Deprecated compatibility escape hatch. When true, every direct
        /// peer is trusted. Prefer a non-empty trusted_peer_cidrs allowlist.
        bool trust_all_direct_peers = false;
        /// IPv4/IPv6 CIDRs permitted to supply RFC 7239 Forwarded headers.
        std::vector<std::string> trusted_peer_cidrs;
        bool apply_proto = true;
        bool apply_host = true;
    };

    TrustedForwardedHeadersMiddleware();
    explicit TrustedForwardedHeadersMiddleware(Config config);

    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;

private:
    [[nodiscard]] bool peer_is_trusted(std::string_view peer) const noexcept;
    Config config_;
};

} // namespace novaboot::middleware
