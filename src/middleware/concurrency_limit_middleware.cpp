#include "novaboot/middleware/concurrency_limit_middleware.h"

#include <stdexcept>
#include <utility>

namespace novaboot::middleware {

ConcurrencyLimitMiddleware::Permit::Permit(ConcurrencyLimitMiddleware* owner,
                                           std::string key) noexcept
    : owner_(owner), key_(std::move(key)) {}

ConcurrencyLimitMiddleware::Permit::Permit(Permit&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), key_(std::move(other.key_)) {}

ConcurrencyLimitMiddleware::Permit&
ConcurrencyLimitMiddleware::Permit::operator=(Permit&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        key_ = std::move(other.key_);
    }
    return *this;
}

ConcurrencyLimitMiddleware::Permit::~Permit() { release(); }

void ConcurrencyLimitMiddleware::Permit::release() noexcept {
    if (owner_ != nullptr) {
        owner_->release(key_);
        owner_ = nullptr;
    }
}

std::string ConcurrencyLimitMiddleware::global_key(const http3::Request&,
                                                    const context::RequestContext&) {
    return "global";
}

std::string ConcurrencyLimitMiddleware::client_address_key(
    const http3::Request& request, const context::RequestContext&) {
    return request.client_address().empty() ? "anonymous" :
        "ip:" + std::string(request.client_address());
}

ConcurrencyLimitMiddleware::ConcurrencyLimitMiddleware()
    : ConcurrencyLimitMiddleware(Config{}) {}

ConcurrencyLimitMiddleware::ConcurrencyLimitMiddleware(Config config)
    : config_(std::move(config)) {
    if (config_.max_concurrent == 0 || config_.max_keys == 0 ||
        config_.max_key_length == 0 || !config_.key_resolver) {
        throw std::invalid_argument("Concurrency limiting requires positive limits and a key resolver");
    }
}

bool ConcurrencyLimitMiddleware::path_allowed(std::string_view path) const {
    for (const auto& pattern : config_.allowlist_paths) {
        if (pattern.ends_with('*')) {
            if (path.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1))) return true;
        } else if (path == pattern) {
            return true;
        }
    }
    return false;
}

ConcurrencyLimitMiddleware::Permit
ConcurrencyLimitMiddleware::try_acquire(std::string key) {
    std::lock_guard lock(mutex_);
    const auto found = active_.find(key);
    if (found == active_.end()) {
        if (active_.size() >= config_.max_keys) return {};
        active_.emplace(key, 1);
        return Permit(this, std::move(key));
    }
    if (found->second >= config_.max_concurrent) return {};
    ++found->second;
    return Permit(this, std::move(key));
}

void ConcurrencyLimitMiddleware::release(std::string_view key) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = active_.find(std::string(key));
    if (found == active_.end()) return;
    if (found->second <= 1) {
        active_.erase(found);
    } else {
        --found->second;
    }
}

void ConcurrencyLimitMiddleware::handle(http3::Request& request, http3::Response& response,
                                        context::RequestContext& context, Next next) {
    if (path_allowed(request.path())) {
        next();
        return;
    }

    auto key = config_.key_resolver(request, context);
    if (key.empty()) key = "anonymous";
    if (key.size() > config_.max_key_length) {
        response.status(400).json(R"({"error":"Bad Request","message":"Concurrency key is too long"})");
        return;
    }

    auto permit = try_acquire(std::move(key));
    if (!permit) {
        response.status(config_.rejected_status).json(config_.rejected_body);
        return;
    }
    next();
}

} // namespace novaboot::middleware
