#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Rate-limit algorithm selected by RateLimitPolicy.
enum class RateLimitAlgorithm {
    TokenBucket,
    FixedWindow,
    SlidingWindow,
    Gcra,
};

/// A rate-limit policy independent of the state backend.
///
/// `limit` requests are permitted per `window`. `burst` applies to token-bucket
/// and GCRA policies; zero uses `limit` as the burst capacity. Fixed-window and
/// sliding-window policies ignore `burst`. `name` must be unique for policies
/// that share one RateLimitStore, so their state cannot collide for one client.
struct RateLimitPolicy {
    std::string name = "default";
    RateLimitAlgorithm algorithm = RateLimitAlgorithm::TokenBucket;
    std::size_t limit = 100;
    std::chrono::milliseconds window = std::chrono::seconds{10};
    std::size_t burst = 0;
};

/// Atomic state backend for configured rate-limit policies.
///
/// A store persists/executes state; it does not choose the algorithm. The
/// policy travels with each acquire operation, so one Redis/Valkey store can
/// serve several middleware policies. Distributed implementations must make
/// each operation atomic and should use their authoritative server clock.
class RateLimitStore {
public:
    using Clock = std::chrono::steady_clock;

    struct Result {
        bool allowed = false;
        std::size_t remaining = 0;
        std::chrono::seconds retry_after = std::chrono::seconds::zero();
    };

    virtual ~RateLimitStore() = default;

    [[nodiscard]] virtual Result acquire(
        std::string_view key, const RateLimitPolicy& policy,
        Clock::time_point now = Clock::now()) = 0;
};

/// Thread-safe, bounded local RateLimitStore implementing every supported
/// RateLimitAlgorithm. It is suitable for one process only.
class InMemoryRateLimitStore final : public RateLimitStore {
public:
    struct Config {
        std::size_t max_keys = 10'000;
        std::chrono::minutes idle_ttl = std::chrono::minutes{10};
    };

    InMemoryRateLimitStore();
    explicit InMemoryRateLimitStore(Config config);

    [[nodiscard]] Result acquire(std::string_view key,
                                 const RateLimitPolicy& policy,
                                 Clock::time_point now = Clock::now()) override;
    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    struct TokenBucketState {
        double tokens = 0.0;
        Clock::time_point updated_at;
    };
    struct FixedWindowState {
        Clock::time_point started_at;
        std::size_t used = 0;
    };
    struct SlidingWindowState {
        std::deque<Clock::time_point> requests;
    };
    struct GcraState {
        Clock::time_point theoretical_arrival;
    };
    using State = std::variant<TokenBucketState, FixedWindowState,
                               SlidingWindowState, GcraState>;
    struct Entry {
        RateLimitAlgorithm algorithm = RateLimitAlgorithm::TokenBucket;
        State state = TokenBucketState{};
        Clock::time_point updated_at;
    };

    void prune_idle(Clock::time_point now);
    [[nodiscard]] static Clock::duration policy_window(const RateLimitPolicy& policy);
    [[nodiscard]] static std::size_t burst_capacity(const RateLimitPolicy& policy);
    [[nodiscard]] static std::chrono::seconds ceil_seconds(Clock::duration duration) noexcept;

    [[nodiscard]] Result acquire_token_bucket(Entry& entry,
                                               const RateLimitPolicy& policy,
                                               Clock::time_point now);
    [[nodiscard]] Result acquire_fixed_window(Entry& entry,
                                               const RateLimitPolicy& policy,
                                               Clock::time_point now);
    [[nodiscard]] Result acquire_sliding_window(Entry& entry,
                                                 const RateLimitPolicy& policy,
                                                 Clock::time_point now);
    [[nodiscard]] Result acquire_gcra(Entry& entry, const RateLimitPolicy& policy,
                                      Clock::time_point now);

    Config config_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

/// Per-process rate-limit middleware.
///
/// `global_key()` is the safe default. `authenticated_principal_key()` creates
/// independent buckets for validated JWT/session identities and a shared
/// anonymous bucket otherwise. Do not derive keys from forwarded client-IP
/// headers unless a separately configured trusted-proxy boundary has validated
/// them.
class RateLimitMiddleware final : public Middleware {
public:
    using KeyResolver = std::function<std::string(
        const http3::Request&, const context::RequestContext&)>;

    static std::string global_key(const http3::Request& request,
                                  const context::RequestContext& context);
    static std::string authenticated_principal_key(
        const http3::Request& request, const context::RequestContext& context);
    /// Key by the transport-provided peer or a CIDR-validated Forwarded `for`.
    static std::string client_address_key(
        const http3::Request& request, const context::RequestContext& context);

    struct Config {
        RateLimitPolicy policy;
        /// Optional shared/distributed state backend. When omitted NovaBoot
        /// creates InMemoryRateLimitStore. The configured policy, not the store,
        /// determines the rate-limit algorithm.
        std::shared_ptr<RateLimitStore> store;
        KeyResolver key_resolver = global_key;
        std::vector<std::string> allowlist_paths;
        std::size_t max_key_length = 256;
        int rejected_status = 429;
        std::string rejected_body =
            R"({"error":"Too Many Requests","message":"Rate limit exceeded"})";
    };

    RateLimitMiddleware();
    explicit RateLimitMiddleware(Config config);

    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;

private:
    [[nodiscard]] bool path_allowed(std::string_view path) const;
    void publish_headers(http3::Response& response,
                         const RateLimitStore::Result& result) const;

    Config config_;
    std::shared_ptr<RateLimitStore> store_;
};

} // namespace novaboot::middleware
