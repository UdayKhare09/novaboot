#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace novaboot::async {

class CancellationRegistration;

/// Copyable observation handle for cooperative cancellation. A token is safe
/// to cancel from another thread; the operation that owns it decides how to
/// interrupt its transport and observes `cancelled()` at safe boundaries.
class CancellationToken {
public:
    /// An empty token is never cancelled and is suitable as an API default.
    CancellationToken() = default;

    [[nodiscard]] bool cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    /// Arrange for `callback` to run once when this token is cancelled. The
    /// callback may run on the cancelling thread; use EventLoop::post before
    /// touching owner-thread transport state.
    [[nodiscard]] CancellationRegistration on_cancel(std::function<void()> callback) const;

private:
    struct State {
        std::atomic_bool cancelled = false;
        std::mutex mutex;
        std::unordered_map<std::size_t, std::function<void()>> callbacks;
        std::size_t next_callback_id = 1;
    };

    explicit CancellationToken(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;

    friend class CancellationSource;
    friend class CancellationRegistration;
};

/// RAII subscription returned by CancellationToken::on_cancel.
class CancellationRegistration {
public:
    CancellationRegistration() = default;
    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;
    CancellationRegistration(CancellationRegistration&& other) noexcept
        : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}
    CancellationRegistration& operator=(CancellationRegistration&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }
    ~CancellationRegistration() { reset(); }

    void reset() noexcept {
        const auto state = state_.lock();
        if (!state || id_ == 0) return;
        std::lock_guard lock(state->mutex);
        state->callbacks.erase(id_);
        id_ = 0;
    }

private:
    explicit CancellationRegistration(std::weak_ptr<CancellationToken::State> state,
                                      std::size_t id)
        : state_(std::move(state)), id_(id) {}
    std::weak_ptr<CancellationToken::State> state_;
    std::size_t id_ = 0;
    friend class CancellationToken;
};

inline CancellationRegistration
CancellationToken::on_cancel(std::function<void()> callback) const {
    if (!callback) return {};
    if (!state_) return {};
    bool invoke_now = false;
    std::size_t id = 0;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelled.load(std::memory_order_acquire)) {
            invoke_now = true;
        } else {
            id = state_->next_callback_id++;
            state_->callbacks.emplace(id, std::move(callback));
        }
    }
    if (invoke_now) callback();
    return CancellationRegistration(state_, id);
}

/// Owns cancellation for one or more operations sharing its token.
class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
    void cancel() const noexcept {
        if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) return;
        std::unordered_map<std::size_t, std::function<void()>> callbacks;
        {
            std::lock_guard lock(state_->mutex);
            callbacks.swap(state_->callbacks);
        }
        for (auto& [_, callback] : callbacks) callback();
    }

private:
    std::shared_ptr<CancellationToken::State> state_;
};

} // namespace novaboot::async
