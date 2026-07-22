#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <variant>

namespace novaboot::async {

/// A lightweight C++20/26 coroutine Task<T>.
///
/// Supports:
///   - `co_await task` — suspends caller until result is ready
///   - `co_return value` — provides result
///   - `co_return` (for Task<void>)
///   - Propagates exceptions thrown inside the coroutine
///
/// Tasks are eagerly started (not lazy). They run until their first
/// suspension point, then resume when their event-loop callback fires.
///
/// Usage:
///   async::Task<ClientResponse> fetch() {
///       co_return co_await rest_client_.async_get("/api/users");
///   }
template<typename T>
class Task {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        std::variant<std::monostate, T, std::exception_ptr> result_;
        std::coroutine_handle<> continuation_;

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }

            std::coroutine_handle<> await_suspend(handle_type handle) const noexcept {
                const auto continuation = handle.promise().continuation_;
                return continuation ? continuation : std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }

        std::suspend_never  initial_suspend() noexcept { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T value) {
            result_ = std::move(value);
        }

        void unhandled_exception() {
            result_ = std::current_exception();
        }
    };

    explicit Task(handle_type h) : handle_(h) {}

    Task(Task&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    Task& operator=(Task&&) = delete;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) handle_.destroy();
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return handle_ && handle_.done();
    }

    // Awaitable interface
    bool await_ready() const noexcept { return handle_.done(); }

    void await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
    }

    T await_resume() {
        auto& result = handle_.promise().result_;
        if (std::holds_alternative<std::exception_ptr>(result)) {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        }
        return std::move(std::get<T>(result));
    }

private:
    handle_type handle_;
};

/// Task<void> specialization
template<>
class Task<void> {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        std::exception_ptr    exception_;
        std::coroutine_handle<> continuation_;

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }

            std::coroutine_handle<> await_suspend(handle_type handle) const noexcept {
                const auto continuation = handle.promise().continuation_;
                return continuation ? continuation : std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }

        std::suspend_never  initial_suspend() noexcept { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() {}

        void unhandled_exception() {
            exception_ = std::current_exception();
        }
    };

    explicit Task(handle_type h) : handle_(h) {}

    Task(Task&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    Task& operator=(Task&&) = delete;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { if (handle_) handle_.destroy(); }

    [[nodiscard]] bool is_ready() const noexcept {
        return handle_ && handle_.done();
    }

    bool await_ready() const noexcept { return handle_.done(); }

    void await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
    }

    void await_resume() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }

private:
    handle_type handle_;
};

/// Awaitable that suspends the coroutine and resumes it via an EventLoop callback.
/// Used internally by RestClient to wait for an HTTP/3 response.
struct EventLoopSuspend {
    std::coroutine_handle<>* out_handle;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        *out_handle = h;
    }

    void await_resume() const noexcept {}
};

} // namespace novaboot::async
