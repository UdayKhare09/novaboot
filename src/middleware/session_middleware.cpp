#include "novaboot/middleware/session_middleware.h"

#include <stdexcept>
#include <utility>

#include <openssl/rand.h>

namespace novaboot::middleware {

void InMemorySessionStore::put(Session session) {
    std::unique_lock lock(mutex_);
    sessions_.insert_or_assign(session.id, std::move(session));
}

std::optional<Session>
InMemorySessionStore::find(std::string_view id, std::chrono::system_clock::time_point now) {
    std::unique_lock lock(mutex_);
    const auto found = sessions_.find(std::string(id));
    if (found == sessions_.end()) return std::nullopt;
    if (found->second.expires_at <= now) {
        sessions_.erase(found);
        return std::nullopt;
    }
    return found->second;
}

void InMemorySessionStore::erase(std::string_view id) {
    std::unique_lock lock(mutex_);
    sessions_.erase(std::string(id));
}

SessionManager::SessionManager() : SessionManager(Config{}) {}

SessionManager::SessionManager(Config config, std::shared_ptr<SessionStore> store)
    : config_(std::move(config)), store_(std::move(store)) {
    if (config_.cookie_name.empty() || config_.cookie_path.empty() ||
        config_.cookie_path.front() != '/' || config_.ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("Session cookie name, absolute path, and positive TTL are required");
    }
    if (config_.same_site == http::SameSite::None && !config_.secure_cookie) {
        throw std::invalid_argument("Session SameSite=None cookie must be Secure");
    }
    if (!store_) store_ = std::make_shared<InMemorySessionStore>();
}

std::string SessionManager::generate_id() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("OpenSSL could not generate a session identifier");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(sizeof(bytes) * 2);
    for (const auto value : bytes) {
        id.push_back(hex[value >> 4]);
        id.push_back(hex[value & 0x0f]);
    }
    return id;
}

void SessionManager::issue_cookie(http3::Response& response, std::string_view id,
                                  std::chrono::seconds max_age) const {
    http::set_cookie(response, http::Cookie{
        .name = config_.cookie_name,
        .value = std::string(id),
        .path = config_.cookie_path,
        .domain = config_.cookie_domain,
        .max_age = max_age,
        .secure = config_.secure_cookie,
        .http_only = true,
        .same_site = config_.same_site,
    });
}

Session SessionManager::login(const http3::Request& request, http3::Response& response,
                              SessionPrincipal principal) {
    if (principal.subject.empty()) {
        throw std::invalid_argument("Session principal subject must not be empty");
    }
    if (const auto previous = http::request_cookie(request, config_.cookie_name)) {
        store_->erase(*previous);
    }

    const auto now = std::chrono::system_clock::now();
    Session session{
        .id = generate_id(),
        .principal = std::move(principal),
        .expires_at = now + config_.ttl,
    };
    store_->put(session);
    issue_cookie(response, session.id, config_.ttl);
    return session;
}

std::optional<SessionPrincipal>
SessionManager::authenticate(const http3::Request& request) const {
    const auto id = http::request_cookie(request, config_.cookie_name);
    if (!id || id->empty()) return std::nullopt;
    const auto session = store_->find(*id, std::chrono::system_clock::now());
    if (!session) return std::nullopt;
    return session->principal;
}

void SessionManager::logout(const http3::Request& request, http3::Response& response) {
    if (const auto id = http::request_cookie(request, config_.cookie_name)) {
        store_->erase(*id);
    }
    issue_cookie(response, "", std::chrono::seconds::zero());
}

SessionMiddleware::SessionMiddleware(std::shared_ptr<SessionManager> sessions)
    : SessionMiddleware(std::move(sessions), Config{}) {}

SessionMiddleware::SessionMiddleware(std::shared_ptr<SessionManager> sessions, Config config)
    : sessions_(std::move(sessions)), config_(std::move(config)) {
    if (!sessions_) throw std::invalid_argument("SessionMiddleware requires a SessionManager");
}

bool SessionMiddleware::path_allowed(std::string_view path) const {
    for (const auto& pattern : config_.allowlist_paths) {
        if (pattern.ends_with('*')) {
            if (path.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1))) return true;
        } else if (path == pattern) {
            return true;
        }
    }
    return false;
}

void SessionMiddleware::handle(http3::Request& request, http3::Response& response,
                               context::RequestContext& context, Next next) {
    if ((config_.allow_options_requests && request.method() == "OPTIONS") ||
        path_allowed(request.path())) {
        next();
        return;
    }
    if (const auto principal = sessions_->authenticate(request)) {
        context.set(*principal);
        next();
        return;
    }
    response.status(config_.unauthorized_status).json(config_.unauthorized_body);
}

} // namespace novaboot::middleware
