#include "novaboot/middleware/rate_limit_middleware.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/session_middleware.h"

namespace novaboot::middleware {
namespace {

void validate_policy(const RateLimitPolicy& policy) {
    if (policy.name.empty() || policy.limit == 0 ||
        policy.window <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Rate-limit policy requires a name, positive limit, and window");
    }
}

} // namespace

InMemoryRateLimitStore::InMemoryRateLimitStore()
    : InMemoryRateLimitStore(Config{}) {}

InMemoryRateLimitStore::InMemoryRateLimitStore(Config config)
    : config_(std::move(config)) {
    if (config_.max_keys == 0 || config_.idle_ttl <= std::chrono::minutes::zero()) {
        throw std::invalid_argument("In-memory rate-limit store requires positive key limit and idle TTL");
    }
}

void InMemoryRateLimitStore::prune_idle(Clock::time_point now) {
    std::erase_if(entries_, [&](const auto& entry) {
        return now >= entry.second.updated_at &&
               now - entry.second.updated_at >= config_.idle_ttl;
    });
}

InMemoryRateLimitStore::Clock::duration
InMemoryRateLimitStore::policy_window(const RateLimitPolicy& policy) {
    validate_policy(policy);
    const auto window = std::chrono::duration_cast<Clock::duration>(policy.window);
    if (window <= Clock::duration::zero()) {
        throw std::invalid_argument("Rate-limit window is too small for the configured clock");
    }
    return window;
}

std::size_t InMemoryRateLimitStore::burst_capacity(const RateLimitPolicy& policy) {
    validate_policy(policy);
    return policy.burst == 0 ? policy.limit : policy.burst;
}

std::chrono::seconds
InMemoryRateLimitStore::ceil_seconds(Clock::duration duration) noexcept {
    if (duration <= Clock::duration::zero()) return std::chrono::seconds::zero();
    const auto seconds = std::ceil(std::chrono::duration<double>(duration).count());
    const auto maximum = static_cast<double>(std::numeric_limits<long long>::max());
    return std::chrono::seconds{static_cast<long long>(std::min(seconds, maximum))};
}

RateLimitStore::Result InMemoryRateLimitStore::acquire_token_bucket(
    Entry& entry, const RateLimitPolicy& policy, Clock::time_point now) {
    const auto window = policy_window(policy);
    const auto capacity = burst_capacity(policy);
    if (!std::holds_alternative<TokenBucketState>(entry.state)) {
        entry.state = TokenBucketState{.tokens = static_cast<double>(capacity), .updated_at = now};
    }
    auto& bucket = std::get<TokenBucketState>(entry.state);
    if (now > bucket.updated_at) {
        const auto elapsed = std::chrono::duration<double>(now - bucket.updated_at).count();
        const auto refill_rate = static_cast<double>(policy.limit) /
            std::chrono::duration<double>(window).count();
        bucket.tokens = std::min(static_cast<double>(capacity),
                                 bucket.tokens + elapsed * refill_rate);
        bucket.updated_at = now;
    }
    if (bucket.tokens < 1.0) {
        const auto refill_rate = static_cast<double>(policy.limit) /
            std::chrono::duration<double>(window).count();
        const auto wait = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>((1.0 - bucket.tokens) / refill_rate));
        return {.allowed = false, .remaining = 0, .retry_after = ceil_seconds(wait)};
    }
    bucket.tokens -= 1.0;
    return {.allowed = true,
            .remaining = static_cast<std::size_t>(std::floor(bucket.tokens)),
            .retry_after = std::chrono::seconds::zero()};
}

RateLimitStore::Result InMemoryRateLimitStore::acquire_fixed_window(
    Entry& entry, const RateLimitPolicy& policy, Clock::time_point now) {
    const auto window = policy_window(policy);
    if (!std::holds_alternative<FixedWindowState>(entry.state)) {
        entry.state = FixedWindowState{.started_at = now};
    }
    auto& bucket = std::get<FixedWindowState>(entry.state);
    if (now >= bucket.started_at && now - bucket.started_at >= window) {
        bucket.started_at = now;
        bucket.used = 0;
    }
    if (bucket.used >= policy.limit) {
        return {.allowed = false, .remaining = 0,
                .retry_after = ceil_seconds(window - (now - bucket.started_at))};
    }
    ++bucket.used;
    return {.allowed = true, .remaining = policy.limit - bucket.used,
            .retry_after = std::chrono::seconds::zero()};
}

RateLimitStore::Result InMemoryRateLimitStore::acquire_sliding_window(
    Entry& entry, const RateLimitPolicy& policy, Clock::time_point now) {
    const auto window = policy_window(policy);
    if (!std::holds_alternative<SlidingWindowState>(entry.state)) {
        entry.state = SlidingWindowState{};
    }
    auto& bucket = std::get<SlidingWindowState>(entry.state);
    while (!bucket.requests.empty() && now >= bucket.requests.front() &&
           now - bucket.requests.front() >= window) {
        bucket.requests.pop_front();
    }
    if (bucket.requests.size() >= policy.limit) {
        const auto retry = bucket.requests.front() + window - now;
        return {.allowed = false, .remaining = 0, .retry_after = ceil_seconds(retry)};
    }
    bucket.requests.push_back(now);
    return {.allowed = true, .remaining = policy.limit - bucket.requests.size(),
            .retry_after = std::chrono::seconds::zero()};
}

RateLimitStore::Result InMemoryRateLimitStore::acquire_gcra(
    Entry& entry, const RateLimitPolicy& policy, Clock::time_point now) {
    const auto window = policy_window(policy);
    const auto interval = window / static_cast<Clock::duration::rep>(policy.limit);
    if (interval <= Clock::duration::zero()) {
        throw std::invalid_argument("GCRA interval is too small for the configured policy");
    }
    const auto burst = burst_capacity(policy);
    const auto tolerance = interval * static_cast<Clock::duration::rep>(burst - 1);
    if (!std::holds_alternative<GcraState>(entry.state)) {
        entry.state = GcraState{.theoretical_arrival = now};
    }
    auto& gcra = std::get<GcraState>(entry.state);
    const auto allowed_at = gcra.theoretical_arrival - tolerance;
    if (now < allowed_at) {
        return {.allowed = false, .remaining = 0,
                .retry_after = ceil_seconds(allowed_at - now)};
    }
    gcra.theoretical_arrival = std::max(gcra.theoretical_arrival, now) + interval;
    const auto debt = gcra.theoretical_arrival > now
        ? gcra.theoretical_arrival - now : Clock::duration::zero();
    const auto used = static_cast<std::size_t>(
        std::ceil(std::chrono::duration<double>(debt).count() /
                  std::chrono::duration<double>(interval).count()));
    return {.allowed = true, .remaining = burst > used ? burst - used : 0,
            .retry_after = std::chrono::seconds::zero()};
}

RateLimitStore::Result InMemoryRateLimitStore::acquire(
    std::string_view key, const RateLimitPolicy& policy, Clock::time_point now) {
    validate_policy(policy);
    std::lock_guard lock(mutex_);
    const std::string_view user_key = key.empty() ? std::string_view{"anonymous"} : key;
    const std::string normalized = std::to_string(policy.name.size()) + ":" +
        policy.name + std::string(user_key);
    auto found = entries_.find(normalized);
    if (found == entries_.end()) {
        if (entries_.size() >= config_.max_keys) {
            prune_idle(now);
            if (entries_.size() >= config_.max_keys) {
                return {.allowed = false, .remaining = 0,
                        .retry_after = std::chrono::seconds{1}};
            }
        }
        found = entries_.emplace(normalized, Entry{
            .algorithm = policy.algorithm,
            .updated_at = now,
        }).first;
    }
    auto& entry = found->second;
    if (entry.algorithm != policy.algorithm) {
        entry.algorithm = policy.algorithm;
        entry.state = TokenBucketState{};
    }
    entry.updated_at = now;

    switch (policy.algorithm) {
    case RateLimitAlgorithm::TokenBucket:
        return acquire_token_bucket(entry, policy, now);
    case RateLimitAlgorithm::FixedWindow:
        return acquire_fixed_window(entry, policy, now);
    case RateLimitAlgorithm::SlidingWindow:
        return acquire_sliding_window(entry, policy, now);
    case RateLimitAlgorithm::Gcra:
        return acquire_gcra(entry, policy, now);
    }
    throw std::invalid_argument("Unsupported rate-limit algorithm");
}

std::string RateLimitMiddleware::global_key(const http3::Request&,
                                             const context::RequestContext&) {
    return "global";
}

std::string RateLimitMiddleware::authenticated_principal_key(
    const http3::Request&, const context::RequestContext& context) {
    if (const auto* jwt = context.get<JwtPrincipal>(); jwt != nullptr && !jwt->subject.empty()) {
        return "jwt:" + jwt->subject;
    }
    if (const auto* session = context.get<SessionPrincipal>();
        session != nullptr && !session->subject.empty()) {
        return "session:" + session->subject;
    }
    return "anonymous";
}

std::string RateLimitMiddleware::client_address_key(
    const http3::Request& request, const context::RequestContext&) {
    return request.client_address().empty() ? "anonymous" :
        "ip:" + std::string(request.client_address());
}

RateLimitMiddleware::RateLimitMiddleware() : RateLimitMiddleware(Config{}) {}

RateLimitMiddleware::RateLimitMiddleware(Config config)
    : config_(std::move(config)), store_(config_.store) {
    validate_policy(config_.policy);
    if (!config_.key_resolver || config_.max_key_length == 0) {
        throw std::invalid_argument("Rate limiting requires a key resolver and positive key length");
    }
    if (!store_) store_ = std::make_shared<InMemoryRateLimitStore>();
}

bool RateLimitMiddleware::path_allowed(std::string_view path) const {
    for (const auto& pattern : config_.allowlist_paths) {
        if (pattern.ends_with('*')) {
            if (path.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1))) return true;
        } else if (path == pattern) {
            return true;
        }
    }
    return false;
}

void RateLimitMiddleware::publish_headers(
    http3::Response& response, const RateLimitStore::Result& result) const {
    response.header("ratelimit-limit", std::to_string(config_.policy.limit));
    response.header("ratelimit-remaining", std::to_string(result.remaining));
    if (!result.allowed) {
        response.header("ratelimit-reset", std::to_string(result.retry_after.count()));
        response.header("retry-after", std::to_string(result.retry_after.count()));
    }
}

void RateLimitMiddleware::handle(http3::Request& request, http3::Response& response,
                                 context::RequestContext& context, Next next) {
    if (path_allowed(request.path())) {
        next();
        return;
    }
    auto key = config_.key_resolver(request, context);
    if (key.empty()) key = "anonymous";
    if (key.size() > config_.max_key_length) {
        response.status(400).json(R"({"error":"Bad Request","message":"Rate limit key is too long"})");
        return;
    }
    const auto result = store_->acquire(key, config_.policy);
    publish_headers(response, result);
    if (!result.allowed) {
        response.status(config_.rejected_status).json(config_.rejected_body);
        return;
    }
    next();
}

} // namespace novaboot::middleware
