#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Bounds active downstream HTTP work rather than request rate.
///
/// A permit is held from this middleware until all later middleware and the
/// route handler return, including when an exception unwinds the pipeline. The
/// default is one global pool; applications may provide a safe identity key to
/// establish independent per-principal pools.
class ConcurrencyLimitMiddleware final : public Middleware {
public:
    using KeyResolver = std::function<std::string(
        const http3::Request&, const context::RequestContext&)>;

    static std::string global_key(const http3::Request& request,
                                  const context::RequestContext& context);
    /// Key by the transport-provided peer or a CIDR-validated Forwarded `for`.
    static std::string client_address_key(const http3::Request& request,
                                          const context::RequestContext& context);

    struct Config {
        std::size_t max_concurrent = 100;
        std::size_t max_keys = 10'000;
        KeyResolver key_resolver = global_key;
        std::vector<std::string> allowlist_paths;
        std::size_t max_key_length = 256;
        int rejected_status = 429;
        std::string rejected_body =
            R"({"error":"Too Many Requests","message":"Concurrency limit exceeded"})";
    };

    ConcurrencyLimitMiddleware();
    explicit ConcurrencyLimitMiddleware(Config config);

    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;

private:
    class Permit {
    public:
        Permit() = default;
        Permit(ConcurrencyLimitMiddleware* owner, std::string key) noexcept;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept;
        Permit& operator=(Permit&& other) noexcept;
        ~Permit();

        explicit operator bool() const noexcept { return owner_ != nullptr; }

    private:
        void release() noexcept;

        ConcurrencyLimitMiddleware* owner_ = nullptr;
        std::string key_;
    };

    [[nodiscard]] bool path_allowed(std::string_view path) const;
    [[nodiscard]] Permit try_acquire(std::string key);
    void release(std::string_view key) noexcept;

    Config config_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::size_t> active_;
};

} // namespace novaboot::middleware
