#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "novaboot/context/request_context.h"
#include "novaboot/middleware/concurrency_limit_middleware.h"

namespace {

novaboot::middleware::ConcurrencyLimitMiddleware::Config concurrency_config(
    std::size_t max_concurrent = 1) {
    auto config = novaboot::middleware::ConcurrencyLimitMiddleware::Config{};
    config.max_concurrent = max_concurrent;
    return config;
}

TEST(ConcurrencyLimitMiddlewareTest, RejectsWhileAllPermitsAreActiveThenReleases) {
    novaboot::middleware::ConcurrencyLimitMiddleware middleware(concurrency_config());
    novaboot::http3::Request first_request;
    first_request.set_method("GET");
    first_request.set_path("/expensive");
    novaboot::http3::Response first_response;
    novaboot::context::RequestContext first_context;

    std::mutex gate_mutex;
    std::condition_variable gate;
    bool entered = false;
    bool release_first = false;
    bool first_called = false;

    std::thread first([&] {
        middleware.handle(first_request, first_response, first_context, [&] {
            std::unique_lock lock(gate_mutex);
            first_called = true;
            entered = true;
            gate.notify_one();
            gate.wait(lock, [&] { return release_first; });
        });
    });

    {
        std::unique_lock lock(gate_mutex);
        gate.wait(lock, [&] { return entered; });
    }

    novaboot::http3::Request second_request;
    second_request.set_method("GET");
    second_request.set_path("/expensive");
    novaboot::http3::Response second_response;
    novaboot::context::RequestContext second_context;
    bool second_called = false;
    middleware.handle(second_request, second_response, second_context,
                      [&] { second_called = true; });
    EXPECT_FALSE(second_called);
    EXPECT_EQ(second_response.status_code(), 429);

    {
        std::lock_guard lock(gate_mutex);
        release_first = true;
    }
    gate.notify_one();
    first.join();
    EXPECT_TRUE(first_called);

    novaboot::http3::Response third_response;
    bool third_called = false;
    middleware.handle(second_request, third_response, second_context,
                      [&] { third_called = true; });
    EXPECT_TRUE(third_called);
    EXPECT_EQ(third_response.status_code(), 200);
}

TEST(ConcurrencyLimitMiddlewareTest, PermitIsReleasedWhenHandlerThrows) {
    novaboot::middleware::ConcurrencyLimitMiddleware middleware(concurrency_config());
    novaboot::http3::Request request;
    request.set_method("GET");
    request.set_path("/expensive");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;

    EXPECT_THROW(middleware.handle(request, response, context, [] {
        throw std::runtime_error("handler failure");
    }), std::runtime_error);

    bool called = false;
    novaboot::http3::Response next_response;
    middleware.handle(request, next_response, context, [&] { called = true; });
    EXPECT_TRUE(called);
}

TEST(ConcurrencyLimitMiddlewareTest, KeysCanUseIndependentPools) {
    auto config = concurrency_config();
    config.key_resolver = [](const auto& request, const auto&) {
        return std::string(*request.header("x-client"));
    };
    novaboot::middleware::ConcurrencyLimitMiddleware middleware(std::move(config));
    novaboot::http3::Request first;
    first.set_method("GET");
    first.set_path("/expensive");
    first.headers().set("x-client", "one");
    novaboot::http3::Request second;
    second.set_method("GET");
    second.set_path("/expensive");
    second.headers().set("x-client", "two");
    novaboot::http3::Response first_response;
    novaboot::context::RequestContext first_context;
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool entered = false;
    bool release_first = false;
    bool first_called = false;
    std::thread first_thread([&] {
        middleware.handle(first, first_response, first_context, [&] {
            std::unique_lock lock(gate_mutex);
            first_called = true;
            entered = true;
            gate.notify_one();
            gate.wait(lock, [&] { return release_first; });
        });
    });
    {
        std::unique_lock lock(gate_mutex);
        gate.wait(lock, [&] { return entered; });
    }

    novaboot::http3::Response second_response;
    novaboot::context::RequestContext second_context;
    bool second_called = false;
    middleware.handle(second, second_response, second_context, [&] { second_called = true; });

    EXPECT_TRUE(first_called);
    EXPECT_TRUE(second_called);

    novaboot::http3::Response same_key_response;
    bool same_key_called = false;
    middleware.handle(first, same_key_response, first_context, [&] { same_key_called = true; });
    EXPECT_FALSE(same_key_called);
    EXPECT_EQ(same_key_response.status_code(), 429);

    {
        std::lock_guard lock(gate_mutex);
        release_first = true;
    }
    gate.notify_one();
    first_thread.join();
}

} // namespace
