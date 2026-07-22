#pragma once

#include <atomic>
#include <memory>

namespace novaboot::async {

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

private:
    struct State {
        std::atomic_bool cancelled = false;
    };

    explicit CancellationToken(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;

    friend class CancellationSource;
};

/// Owns cancellation for one or more operations sharing its token.
class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
    void cancel() const noexcept { state_->cancelled.store(true, std::memory_order_release); }

private:
    std::shared_ptr<CancellationToken::State> state_;
};

} // namespace novaboot::async
