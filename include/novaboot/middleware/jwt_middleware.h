#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "novaboot/middleware/middleware.h"

namespace novaboot::middleware {

enum class JwtAlgorithm {
    HS256,
    RS256,
};

/// Structured JWT claims made available to downstream handlers through
/// RequestContext::get<JwtPrincipal>().
struct JwtClaims {
    std::unordered_map<std::string, std::string> string_claims;
    std::unordered_map<std::string, std::int64_t> integer_claims;
    std::unordered_map<std::string, bool> bool_claims;
    std::unordered_map<std::string, std::vector<std::string>> string_array_claims;

    [[nodiscard]] std::optional<std::string_view>
    string(std::string_view name) const;

    [[nodiscard]] std::optional<std::int64_t>
    integer(std::string_view name) const;

    [[nodiscard]] std::optional<bool>
    boolean(std::string_view name) const;

    [[nodiscard]] const std::vector<std::string>*
    string_array(std::string_view name) const;
};

struct JwtPrincipal {
    std::string subject;
    std::string issuer;
    std::vector<std::string> audience;
    std::vector<std::string> scopes;
    std::string token_id;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::optional<std::chrono::system_clock::time_point> not_before;
    std::optional<std::chrono::system_clock::time_point> issued_at;
    JwtClaims claims;
};

/// Fluent builder for compact JWT payload claims.
class JwtTokenBuilder {
public:
    JwtTokenBuilder& subject(std::string value);
    JwtTokenBuilder& issuer(std::string value);
    JwtTokenBuilder& audience(std::string value);
    JwtTokenBuilder& audiences(std::vector<std::string> values);
    JwtTokenBuilder& scope(std::string value);
    JwtTokenBuilder& scopes(std::vector<std::string> values);
    JwtTokenBuilder& token_id(std::string value);
    JwtTokenBuilder& expires_at(std::chrono::system_clock::time_point value);
    JwtTokenBuilder& expires_in(std::chrono::seconds value);
    JwtTokenBuilder& not_before(std::chrono::system_clock::time_point value);
    JwtTokenBuilder& issued_at(std::chrono::system_clock::time_point value);
    JwtTokenBuilder& issued_now();

    JwtTokenBuilder& claim(std::string name, std::string value);
    JwtTokenBuilder& claim(std::string name, std::string_view value);
    JwtTokenBuilder& claim(std::string name, const char* value);
    JwtTokenBuilder& claim(std::string name, std::int64_t value);
    JwtTokenBuilder& claim(std::string name, bool value);
    JwtTokenBuilder& claim(std::string name, std::vector<std::string> value);

    [[nodiscard]] const JwtClaims& claims() const noexcept { return claims_; }
    [[nodiscard]] const std::optional<std::string>& subject() const noexcept { return subject_; }
    [[nodiscard]] const std::optional<std::string>& issuer() const noexcept { return issuer_; }
    [[nodiscard]] const std::vector<std::string>& audience() const noexcept { return audience_; }
    [[nodiscard]] const std::vector<std::string>& scopes() const noexcept { return scopes_; }
    [[nodiscard]] const std::optional<std::string>& token_id() const noexcept { return token_id_; }
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> expires_at() const noexcept { return expires_at_; }
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> not_before() const noexcept { return not_before_; }
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> issued_at() const noexcept { return issued_at_; }

private:
    std::optional<std::string> subject_;
    std::optional<std::string> issuer_;
    std::vector<std::string> audience_;
    std::vector<std::string> scopes_;
    std::optional<std::string> token_id_;
    std::optional<std::chrono::system_clock::time_point> expires_at_;
    std::optional<std::chrono::system_clock::time_point> not_before_;
    std::optional<std::chrono::system_clock::time_point> issued_at_;
    JwtClaims claims_;
};

/// JWT compact-token issuer for HS256 and RS256 tokens.
class JwtIssuer {
public:
    using Algorithm = JwtAlgorithm;

    struct Config {
        Algorithm algorithm = Algorithm::HS256;
        std::string hmac_secret;
        std::string rsa_private_key_pem;
        std::string key_id;
        std::string type = "JWT";
        std::string scope_claim = "scope";
        bool encode_scopes_as_array = false;
        bool include_issued_at = true;
        std::chrono::seconds default_ttl = std::chrono::hours{1};
    };

    explicit JwtIssuer(Config cfg);

    [[nodiscard]] std::expected<std::string, std::string>
    issue(const JwtTokenBuilder& token) const;

    [[nodiscard]] static std::expected<std::string, std::string>
    issue(const Config& cfg, const JwtTokenBuilder& token);

private:
    Config cfg_;
};

/// JWT authentication middleware.
///
/// Supports HS256 and RS256 compact JWTs from an Authorization: Bearer header,
/// validates standard temporal claims, optional issuer/audience/scope policy,
/// and stores a JwtPrincipal in the request context for route handlers.
class JwtMiddleware : public Middleware {
public:
    using Algorithm = JwtAlgorithm;

    struct Config {
        std::vector<Algorithm> allowed_algorithms = {
            Algorithm::HS256,
            Algorithm::RS256,
        };

        /// Shared secret for HS256 tokens.
        std::string hmac_secret;

        /// PEM-encoded public key for RS256 tokens.
        std::string rsa_public_key_pem;

        /// Paths that bypass authentication. A trailing '*' matches by prefix.
        std::vector<std::string> allowlist_paths = {};
        bool allow_options_requests = true;

        std::string authorization_header = "authorization";
        std::string bearer_prefix = "Bearer ";

        std::optional<std::string> required_issuer;
        std::vector<std::string> required_audiences = {};
        std::vector<std::string> required_scopes = {};
        std::unordered_map<std::string, std::string> required_claims = {};

        bool require_expiration = true;
        bool validate_not_before = true;
        bool validate_issued_at = true;
        std::chrono::seconds clock_skew = std::chrono::seconds{60};

        std::string scope_claim = "scope";
        int unauthorized_status = 401;
        std::string unauthorized_body =
            R"({"error":"Unauthorized","message":"Invalid or missing bearer token"})";
    };

    JwtMiddleware();
    explicit JwtMiddleware(Config cfg);

    void handle(http3::Request& req,
                http3::Response& res,
                context::RequestContext& ctx,
                Next next) override;

private:
    Config cfg_;
};

} // namespace novaboot::middleware
