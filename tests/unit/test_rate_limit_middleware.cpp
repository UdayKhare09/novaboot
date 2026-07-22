#include <gtest/gtest.h>

#include <memory>

#include "novaboot/context/request_context.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/rate_limit_middleware.h"

namespace {

using RateLimitAlgorithm = novaboot::middleware::RateLimitAlgorithm;
using RateLimitMiddleware = novaboot::middleware::RateLimitMiddleware;
using RateLimitPolicy = novaboot::middleware::RateLimitPolicy;
using RateLimitStore = novaboot::middleware::RateLimitStore;

RateLimitMiddleware::Config rate_config(std::size_t limit = 1) {
    auto config = RateLimitMiddleware::Config{};
    config.policy.limit = limit;
    config.policy.window = std::chrono::seconds{1};
    config.policy.burst = limit;
    return config;
}

novaboot::middleware::JwtPrincipal jwt_principal(std::string subject) {
    novaboot::middleware::JwtPrincipal principal;
    principal.subject = std::move(subject);
    return principal;
}

class RecordingStore final : public RateLimitStore {
public:
    Result acquire(std::string_view key, const RateLimitPolicy& policy,
                   Clock::time_point) override {
        seen_key = std::string(key);
        seen_algorithm = policy.algorithm;
        ++calls;
        return {.allowed = false, .remaining = 0, .retry_after = std::chrono::seconds{7}};
    }

    int calls = 0;
    std::string seen_key;
    RateLimitAlgorithm seen_algorithm = RateLimitAlgorithm::TokenBucket;
};

TEST(InMemoryRateLimitStoreTest, TokenBucketRefillsAtConfiguredRate) {
    novaboot::middleware::InMemoryRateLimitStore store;
    const RateLimitPolicy policy{
        .name = "token-test",
        .algorithm = RateLimitAlgorithm::TokenBucket,
        .limit = 1,
        .window = std::chrono::seconds{1},
        .burst = 2,
    };
    const auto start = RateLimitStore::Clock::now();

    EXPECT_TRUE(store.acquire("client", policy, start).allowed);
    EXPECT_EQ(store.acquire("client", policy, start).remaining, 0U);
    const auto limited = store.acquire("client", policy, start);
    EXPECT_FALSE(limited.allowed);
    EXPECT_EQ(limited.retry_after, std::chrono::seconds{1});
    EXPECT_TRUE(store.acquire("client", policy, start + std::chrono::seconds{1}).allowed);
}

TEST(InMemoryRateLimitStoreTest, FixedAndSlidingWindowsHaveDifferentBoundarySemantics) {
    novaboot::middleware::InMemoryRateLimitStore store;
    const auto start = RateLimitStore::Clock::now();
    const RateLimitPolicy fixed{
        .name = "fixed-test",
        .algorithm = RateLimitAlgorithm::FixedWindow,
        .limit = 2,
        .window = std::chrono::seconds{1},
    };
    const RateLimitPolicy sliding{
        .name = "sliding-test",
        .algorithm = RateLimitAlgorithm::SlidingWindow,
        .limit = 2,
        .window = std::chrono::seconds{1},
    };

    EXPECT_TRUE(store.acquire("fixed", fixed, start).allowed);
    EXPECT_TRUE(store.acquire("fixed", fixed, start).allowed);
    EXPECT_FALSE(store.acquire("fixed", fixed, start).allowed);
    EXPECT_TRUE(store.acquire("fixed", fixed, start + std::chrono::seconds{1}).allowed);

    EXPECT_TRUE(store.acquire("sliding", sliding, start).allowed);
    EXPECT_TRUE(store.acquire("sliding", sliding, start).allowed);
    EXPECT_FALSE(store.acquire("sliding", sliding, start).allowed);
    EXPECT_TRUE(store.acquire("sliding", sliding, start + std::chrono::seconds{1}).allowed);
}

TEST(InMemoryRateLimitStoreTest, GcraAllowsConfiguredBurstThenPacesRequests) {
    novaboot::middleware::InMemoryRateLimitStore store;
    const RateLimitPolicy policy{
        .name = "gcra-test",
        .algorithm = RateLimitAlgorithm::Gcra,
        .limit = 1,
        .window = std::chrono::seconds{1},
        .burst = 2,
    };
    const auto start = RateLimitStore::Clock::now();

    EXPECT_TRUE(store.acquire("client", policy, start).allowed);
    EXPECT_TRUE(store.acquire("client", policy, start).allowed);
    const auto limited = store.acquire("client", policy, start);
    EXPECT_FALSE(limited.allowed);
    EXPECT_EQ(limited.retry_after, std::chrono::seconds{1});
    EXPECT_TRUE(store.acquire("client", policy, start + std::chrono::seconds{1}).allowed);
}

TEST(InMemoryRateLimitStoreTest, SharedStoreKeepsPoliciesIndependent) {
    novaboot::middleware::InMemoryRateLimitStore store;
    const auto start = RateLimitStore::Clock::now();
    const RateLimitPolicy read_policy{
        .name = "read-api",
        .algorithm = RateLimitAlgorithm::FixedWindow,
        .limit = 1,
        .window = std::chrono::seconds{1},
    };
    const RateLimitPolicy write_policy{
        .name = "write-api",
        .algorithm = RateLimitAlgorithm::FixedWindow,
        .limit = 1,
        .window = std::chrono::seconds{1},
    };

    EXPECT_TRUE(store.acquire("user-1", read_policy, start).allowed);
    EXPECT_FALSE(store.acquire("user-1", read_policy, start).allowed);
    EXPECT_TRUE(store.acquire("user-1", write_policy, start).allowed);
}

TEST(RateLimitMiddlewareTest, ReturnsPredictable429AndHeaders) {
    RateLimitMiddleware middleware(rate_config());
    novaboot::http3::Request request;
    request.set_method("GET");
    request.set_path("/api/articles");
    novaboot::context::RequestContext context;
    bool called = false;

    novaboot::http3::Response first;
    middleware.handle(request, first, context, [&] { called = true; });
    EXPECT_TRUE(called);
    EXPECT_EQ(*first.headers().get("ratelimit-limit"), "1");
    EXPECT_EQ(*first.headers().get("ratelimit-remaining"), "0");

    called = false;
    novaboot::http3::Response second;
    middleware.handle(request, second, context, [&] { called = true; });
    EXPECT_FALSE(called);
    EXPECT_EQ(second.status_code(), 429);
    EXPECT_EQ(*second.headers().get("ratelimit-remaining"), "0");
    EXPECT_EQ(*second.headers().get("retry-after"), "1");
}

TEST(RateLimitMiddlewareTest, AuthenticatedPrincipalGetsItsOwnBucket) {
    auto config = rate_config();
    config.key_resolver = RateLimitMiddleware::authenticated_principal_key;
    RateLimitMiddleware middleware(std::move(config));
    novaboot::http3::Request request;
    request.set_method("GET");
    request.set_path("/api/articles");
    novaboot::context::RequestContext first_context;
    first_context.set<novaboot::middleware::JwtPrincipal>(jwt_principal("one"));
    novaboot::context::RequestContext second_context;
    second_context.set<novaboot::middleware::JwtPrincipal>(jwt_principal("two"));

    bool first_called = false;
    novaboot::http3::Response first_response;
    middleware.handle(request, first_response, first_context, [&] { first_called = true; });
    EXPECT_TRUE(first_called);

    bool second_called = false;
    novaboot::http3::Response second_response;
    middleware.handle(request, second_response, second_context, [&] { second_called = true; });
    EXPECT_TRUE(second_called);
}

TEST(RateLimitMiddlewareTest, ClientAddressKeyUsesOnlyTransportOrTrustedProxyAddress) {
    novaboot::http3::Request request;
    novaboot::context::RequestContext context;
    EXPECT_EQ(RateLimitMiddleware::client_address_key(request, context), "anonymous");

    request.set_peer_address("10.0.0.8");
    EXPECT_EQ(RateLimitMiddleware::client_address_key(request, context), "ip:10.0.0.8");

    request.set_client_address("198.51.100.9"); // Set only by trusted forwarding middleware in production.
    EXPECT_EQ(RateLimitMiddleware::client_address_key(request, context), "ip:198.51.100.9");
}

TEST(RateLimitMiddlewareTest, AllowlistedPathIsNotLimited) {
    auto config = rate_config();
    config.allowlist_paths = {"/actuator/*"};
    RateLimitMiddleware middleware(std::move(config));
    novaboot::http3::Request request;
    request.set_method("GET");
    request.set_path("/actuator/health");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;

    middleware.handle(request, response, context, [&] { called = true; });
    middleware.handle(request, response, context, [&] { called = true; });
    EXPECT_TRUE(called);
    EXPECT_EQ(response.status_code(), 200);
}

TEST(RateLimitMiddlewareTest, PassesTheConfiguredPolicyToTheStore) {
    auto store = std::make_shared<RecordingStore>();
    auto config = rate_config();
    config.policy.algorithm = RateLimitAlgorithm::Gcra;
    config.store = store;
    RateLimitMiddleware middleware(std::move(config));
    novaboot::http3::Request request;
    request.set_method("GET");
    request.set_path("/api/articles");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;

    middleware.handle(request, response, context, [&] { called = true; });

    EXPECT_FALSE(called);
    EXPECT_EQ(store->calls, 1);
    EXPECT_EQ(store->seen_key, "global");
    EXPECT_EQ(store->seen_algorithm, RateLimitAlgorithm::Gcra);
    EXPECT_EQ(response.status_code(), 429);
    EXPECT_EQ(*response.headers().get("retry-after"), "7");
}

} // namespace
