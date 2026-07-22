#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "novaboot/http3/cookie.h"
#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

/// Identity stored in a server-side browser session.
struct SessionPrincipal {
    std::string subject;
    std::vector<std::string> roles;
    std::vector<std::string> scopes;
};

/// Server-side session record. The opaque `id` is only for session-store and
/// cookie plumbing; applications should use `SessionPrincipal` in handlers.
struct Session {
    std::string id;
    SessionPrincipal principal;
    std::chrono::system_clock::time_point expires_at;
};

/// Thread-safe in-memory store suitable for a single NovaBoot process.
///
/// The interface is intentionally small so applications can replace it with a
/// durable, shared store before running multiple application instances.
class SessionStore {
public:
    virtual ~SessionStore() = default;

    virtual void put(Session session) = 0;
    [[nodiscard]] virtual std::optional<Session>
    find(std::string_view id, std::chrono::system_clock::time_point now) = 0;
    virtual void erase(std::string_view id) = 0;
};

class InMemorySessionStore final : public SessionStore {
public:
    void put(Session session) override;
    [[nodiscard]] std::optional<Session>
    find(std::string_view id, std::chrono::system_clock::time_point now) override;
    void erase(std::string_view id) override;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;
};

/// Creates, rotates, validates, and clears opaque browser sessions.
///
/// Login always invalidates any incoming session ID before issuing a fresh one,
/// preventing session fixation. Sessions expire server-side as well as through
/// the browser Max-Age cookie attribute.
class SessionManager {
public:
    struct Config {
        std::string cookie_name = "NOVA_SESSION";
        std::string cookie_path = "/";
        std::optional<std::string> cookie_domain;
        std::chrono::seconds ttl = std::chrono::hours{8};
        bool secure_cookie = true;
        http::SameSite same_site = http::SameSite::Lax;
    };

    SessionManager();
    explicit SessionManager(Config config,
                            std::shared_ptr<SessionStore> store = {});

    /// Replace any inbound session with a fresh opaque ID and set its cookie.
    Session login(const http3::Request& request, http3::Response& response,
                  SessionPrincipal principal);

    /// Resolve a non-expired session from the configured request cookie.
    [[nodiscard]] std::optional<SessionPrincipal>
    authenticate(const http3::Request& request) const;

    /// Remove the inbound session and expire the browser cookie immediately.
    void logout(const http3::Request& request, http3::Response& response);

    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    [[nodiscard]] static std::string generate_id();
    void issue_cookie(http3::Response& response, std::string_view id,
                      std::chrono::seconds max_age) const;

    Config config_;
    std::shared_ptr<SessionStore> store_;
};

/// Authenticates cookie sessions and places SessionPrincipal in RequestContext.
/// Configure allowlisted paths for login, logout, health, and other public
/// endpoints. Place CsrfMiddleware after this middleware for browser routes.
class SessionMiddleware final : public Middleware {
public:
    struct Config {
        std::vector<std::string> allowlist_paths;
        bool allow_options_requests = true;
        int unauthorized_status = 401;
        std::string unauthorized_body =
            R"({"error":"Unauthorized","message":"Invalid or expired session"})";
    };

    explicit SessionMiddleware(std::shared_ptr<SessionManager> sessions);
    SessionMiddleware(std::shared_ptr<SessionManager> sessions, Config config);

    void handle(http3::Request& request, http3::Response& response,
                context::RequestContext& context, Next next) override;

private:
    [[nodiscard]] bool path_allowed(std::string_view path) const;

    std::shared_ptr<SessionManager> sessions_;
    Config config_;
};

} // namespace novaboot::middleware
