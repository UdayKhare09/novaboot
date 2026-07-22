#pragma once

#include <atomic>
#include "novaboot/middleware/middleware.h"

namespace novaboot::core {

class HttpDrainGate final : public middleware::Middleware {
public:
    void stop_accepting() noexcept { accepting_.store(false, std::memory_order_release); }
    [[nodiscard]] bool drained() const noexcept { return active_.load(std::memory_order_acquire) == 0; }
    [[nodiscard]] std::size_t active() const noexcept { return active_.load(std::memory_order_acquire); }

    void handle(http3::Request&, http3::Response& response,
                context::RequestContext&, Next next) override {
        if (!accepting_.load(std::memory_order_acquire)) {
            response.status(503).json("{\"error\":\"Server is shutting down\"}");
            return;
        }
        active_.fetch_add(1, std::memory_order_acq_rel);
        struct Guard { std::atomic_size_t& value; ~Guard() { value.fetch_sub(1, std::memory_order_acq_rel); } } guard{active_};
        next();
    }

private:
    std::atomic_bool accepting_{true};
    std::atomic_size_t active_{0};
};

} // namespace novaboot::core
