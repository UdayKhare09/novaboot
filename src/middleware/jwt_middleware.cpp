#include "novaboot/middleware/jwt_middleware.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <utility>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <simdjson.h>

namespace novaboot::middleware {
namespace {

using TimePoint = std::chrono::system_clock::time_point;

struct ParsedToken {
    std::string header_json;
    std::string payload_json;
    std::string signature;
    std::string signing_input;
};

struct VerifiedPayload {
    std::string algorithm;
    JwtPrincipal principal;
};

using Error = std::string_view;

std::int64_t unix_seconds(TimePoint value) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        value.time_since_epoch()).count();
}

std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 2);

    for (const char ch : text) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                escaped += std::format("\\u{:04x}", static_cast<unsigned int>(ch));
            } else {
                escaped.push_back(ch);
            }
        }
    }

    return escaped;
}

std::string json_string(std::string_view text) {
    return "\"" + json_escape(text) + "\"";
}

std::string json_string_array(const std::vector<std::string>& values) {
    std::string json = "[";
    bool first = true;
    for (const auto& value : values) {
        if (!first) json += ",";
        first = false;
        json += json_string(value);
    }
    json += "]";
    return json;
}

std::string space_join(const std::vector<std::string>& values) {
    std::string joined;
    bool first = true;
    for (const auto& value : values) {
        if (!first) joined += " ";
        first = false;
        joined += value;
    }
    return joined;
}

void add_json_field(std::string& json, std::string_view name,
                    std::string value, bool& first) {
    if (!first) json += ",";
    first = false;
    json += json_string(name);
    json += ":";
    json += std::move(value);
}

std::string base64url_encode(std::string_view data) {
    std::string encoded;
    encoded.resize(((data.size() + 2) / 3) * 4);

    const int len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size()));

    encoded.resize(static_cast<std::size_t>(len));
    for (auto& ch : encoded) {
        if (ch == '+') ch = '-';
        if (ch == '/') ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

bool path_allowed(std::string_view path,
                  const std::vector<std::string>& allowlist) {
    for (const auto& pattern : allowlist) {
        if (pattern.ends_with('*')) {
            const std::string_view prefix(pattern.data(), pattern.size() - 1);
            if (path.starts_with(prefix)) return true;
        } else if (path == pattern) {
            return true;
        }
    }
    return false;
}

std::expected<std::string, Error> base64url_decode(std::string_view input) {
    std::string b64;
    b64.reserve(input.size() + 4);

    for (const char ch : input) {
        if (ch == '-') {
            b64.push_back('+');
        } else if (ch == '_') {
            b64.push_back('/');
        } else if (std::isalnum(static_cast<unsigned char>(ch)) ||
                   ch == '=') {
            b64.push_back(ch);
        } else {
            return std::unexpected("invalid_base64url");
        }
    }

    while (b64.size() % 4 != 0) {
        b64.push_back('=');
    }

    std::string decoded;
    decoded.resize((b64.size() / 4) * 3);
    const int len = EVP_DecodeBlock(
        reinterpret_cast<unsigned char*>(decoded.data()),
        reinterpret_cast<const unsigned char*>(b64.data()),
        static_cast<int>(b64.size()));

    if (len < 0) return std::unexpected("invalid_base64");

    auto padding = std::ranges::count(b64, '=');
    decoded.resize(static_cast<std::size_t>(len) -
                   static_cast<std::size_t>(padding));
    return decoded;
}

std::expected<ParsedToken, Error> parse_compact_jwt(std::string_view token) {
    const auto first_dot = token.find('.');
    if (first_dot == std::string_view::npos) {
        return std::unexpected("malformed_token");
    }

    const auto second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos ||
        token.find('.', second_dot + 1) != std::string_view::npos) {
        return std::unexpected("malformed_token");
    }

    const auto encoded_header = token.substr(0, first_dot);
    const auto encoded_payload =
        token.substr(first_dot + 1, second_dot - first_dot - 1);
    const auto encoded_signature = token.substr(second_dot + 1);

    auto header = base64url_decode(encoded_header);
    auto payload = base64url_decode(encoded_payload);
    auto signature = base64url_decode(encoded_signature);

    if (!header || !payload || !signature) {
        return std::unexpected("malformed_token");
    }

    return ParsedToken{
        .header_json = std::move(*header),
        .payload_json = std::move(*payload),
        .signature = std::move(*signature),
        .signing_input = std::string(token.substr(0, second_dot)),
    };
}

std::optional<std::string> get_json_string(simdjson::dom::element obj,
                                           std::string_view key) {
    std::string_view value;
    if (obj[key].get(value) == simdjson::SUCCESS) {
        return std::string(value);
    }
    return std::nullopt;
}

std::optional<std::int64_t> get_json_int(simdjson::dom::element obj,
                                         std::string_view key) {
    std::int64_t value = 0;
    if (obj[key].get(value) == simdjson::SUCCESS) return value;
    return std::nullopt;
}

std::vector<std::string> read_string_or_array(simdjson::dom::element obj,
                                              std::string_view key) {
    std::vector<std::string> values;

    std::string_view single;
    if (obj[key].get(single) == simdjson::SUCCESS) {
        values.emplace_back(single);
        return values;
    }

    simdjson::dom::array array;
    if (obj[key].get(array) == simdjson::SUCCESS) {
        for (auto item : array) {
            std::string_view value;
            if (item.get(value) == simdjson::SUCCESS) {
                values.emplace_back(value);
            }
        }
    }

    return values;
}

std::vector<std::string> split_space_separated(std::string_view text) {
    std::vector<std::string> values;
    std::size_t start = 0;

    while (start < text.size()) {
        while (start < text.size() &&
               std::isspace(static_cast<unsigned char>(text[start]))) {
            ++start;
        }

        auto end = start;
        while (end < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[end]))) {
            ++end;
        }

        if (end > start) values.emplace_back(text.substr(start, end - start));
        start = end;
    }

    return values;
}

std::vector<std::string> read_scopes(simdjson::dom::element payload,
                                     std::string_view claim) {
    std::string_view scope_text;
    if (payload[claim].get(scope_text) == simdjson::SUCCESS) {
        return split_space_separated(scope_text);
    }
    return read_string_or_array(payload, claim);
}

TimePoint from_unix_seconds(std::int64_t seconds) {
    return TimePoint{std::chrono::seconds{seconds}};
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
    return std::ranges::any_of(values, [needle](const std::string& value) {
        return value == needle;
    });
}

bool algorithm_allowed(const JwtMiddleware::Config& cfg, std::string_view alg) {
    for (const auto allowed : cfg.allowed_algorithms) {
        if (allowed == JwtMiddleware::Algorithm::HS256 && alg == "HS256") {
            return true;
        }
        if (allowed == JwtMiddleware::Algorithm::RS256 && alg == "RS256") {
            return true;
        }
    }
    return false;
}

bool constant_time_equal(std::span<const unsigned char> a,
                         std::span<const unsigned char> b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

std::expected<void, Error> verify_hs256(const ParsedToken& token,
                                        std::string_view secret) {
    if (secret.empty()) return std::unexpected("missing_hmac_secret");

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;

    const auto* result = HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(token.signing_input.data()),
        token.signing_input.size(),
        digest.data(),
        &digest_len);

    if (result == nullptr) return std::unexpected("signature_error");

    const auto actual = std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(token.signature.data()),
        token.signature.size());
    const auto expected = std::span<const unsigned char>(
        digest.data(),
        static_cast<std::size_t>(digest_len));

    if (!constant_time_equal(actual, expected)) {
        return std::unexpected("invalid_signature");
    }
    return {};
}

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

std::expected<std::string, std::string> sign_hs256(std::string_view input,
                                                   std::string_view secret) {
    if (secret.empty()) return std::unexpected("missing_hmac_secret");

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;

    const auto* result = HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(input.data()),
        input.size(),
        digest.data(),
        &digest_len);

    if (result == nullptr) return std::unexpected("signature_error");
    return std::string(reinterpret_cast<const char*>(digest.data()), digest_len);
}

std::expected<std::string, std::string> sign_rs256(std::string_view input,
                                                   std::string_view private_key_pem) {
    if (private_key_pem.empty()) return std::unexpected("missing_rsa_private_key");

    BioPtr bio(BIO_new_mem_buf(private_key_pem.data(),
                               static_cast<int>(private_key_pem.size())),
               BIO_free);
    if (!bio) return std::unexpected("signature_error");

    PkeyPtr private_key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
                        EVP_PKEY_free);
    if (!private_key) return std::unexpected("invalid_rsa_private_key");

    MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) return std::unexpected("signature_error");

    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                           private_key.get()) != 1) {
        return std::unexpected("signature_error");
    }

    if (EVP_DigestSignUpdate(ctx.get(), input.data(), input.size()) != 1) {
        return std::unexpected("signature_error");
    }

    std::size_t signature_len = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &signature_len) != 1) {
        return std::unexpected("signature_error");
    }

    std::string signature(signature_len, '\0');
    if (EVP_DigestSignFinal(
            ctx.get(),
            reinterpret_cast<unsigned char*>(signature.data()),
            &signature_len) != 1) {
        return std::unexpected("signature_error");
    }

    signature.resize(signature_len);
    return signature;
}

std::string algorithm_name(JwtAlgorithm algorithm) {
    switch (algorithm) {
    case JwtMiddleware::Algorithm::HS256: return "HS256";
    case JwtMiddleware::Algorithm::RS256: return "RS256";
    }
    return "HS256";
}

std::string serialize_claims(const JwtTokenBuilder& token,
                             const JwtIssuer::Config& cfg) {
    std::string json = "{";
    bool first = true;

    if (token.subject()) add_json_field(json, "sub", json_string(*token.subject()), first);
    if (token.issuer()) add_json_field(json, "iss", json_string(*token.issuer()), first);
    if (!token.audience().empty()) {
        if (token.audience().size() == 1) {
            add_json_field(json, "aud", json_string(token.audience().front()), first);
        } else {
            add_json_field(json, "aud", json_string_array(token.audience()), first);
        }
    }
    if (!token.scopes().empty()) {
        const auto scopes_json = cfg.encode_scopes_as_array
            ? json_string_array(token.scopes())
            : json_string(space_join(token.scopes()));
        add_json_field(json, cfg.scope_claim, scopes_json, first);
    }
    if (token.token_id()) add_json_field(json, "jti", json_string(*token.token_id()), first);

    const auto now = std::chrono::system_clock::now();
    const auto iat = token.issued_at().or_else([&]() -> std::optional<TimePoint> {
        if (cfg.include_issued_at) return now;
        return std::nullopt;
    });
    const auto exp = token.expires_at().or_else([&]() -> std::optional<TimePoint> {
        if (cfg.default_ttl.count() > 0) return now + cfg.default_ttl;
        return std::nullopt;
    });

    if (exp) add_json_field(json, "exp", std::to_string(unix_seconds(*exp)), first);
    if (token.not_before()) add_json_field(json, "nbf", std::to_string(unix_seconds(*token.not_before())), first);
    if (iat) add_json_field(json, "iat", std::to_string(unix_seconds(*iat)), first);

    for (const auto& [name, value] : token.claims().string_claims) {
        add_json_field(json, name, json_string(value), first);
    }
    for (const auto& [name, value] : token.claims().integer_claims) {
        add_json_field(json, name, std::to_string(value), first);
    }
    for (const auto& [name, value] : token.claims().bool_claims) {
        add_json_field(json, name, value ? "true" : "false", first);
    }
    for (const auto& [name, value] : token.claims().string_array_claims) {
        add_json_field(json, name, json_string_array(value), first);
    }

    json += "}";
    return json;
}

std::string serialize_header(const JwtIssuer::Config& cfg) {
    std::string json = "{";
    bool first = true;
    add_json_field(json, "alg", json_string(algorithm_name(cfg.algorithm)), first);
    add_json_field(json, "typ", json_string(cfg.type), first);
    if (!cfg.key_id.empty()) add_json_field(json, "kid", json_string(cfg.key_id), first);
    json += "}";
    return json;
}

std::expected<void, Error> verify_rs256(const ParsedToken& token,
                                        std::string_view public_key_pem) {
    if (public_key_pem.empty()) return std::unexpected("missing_rsa_key");

    BioPtr bio(BIO_new_mem_buf(public_key_pem.data(),
                               static_cast<int>(public_key_pem.size())),
               BIO_free);
    if (!bio) return std::unexpected("signature_error");

    PkeyPtr public_key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr),
                       EVP_PKEY_free);
    if (!public_key) return std::unexpected("invalid_rsa_key");

    MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) return std::unexpected("signature_error");

    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                             public_key.get()) != 1) {
        return std::unexpected("signature_error");
    }

    if (EVP_DigestVerifyUpdate(ctx.get(), token.signing_input.data(),
                               token.signing_input.size()) != 1) {
        return std::unexpected("signature_error");
    }

    const auto ok = EVP_DigestVerifyFinal(
        ctx.get(),
        reinterpret_cast<const unsigned char*>(token.signature.data()),
        token.signature.size());

    if (ok != 1) return std::unexpected("invalid_signature");
    return {};
}

void read_all_claims(simdjson::dom::element payload, JwtClaims& claims) {
    simdjson::dom::object object;
    if (payload.get(object) != simdjson::SUCCESS) return;

    for (auto [key, value] : object) {
        const auto name = std::string(key);

        std::string_view string_value;
        if (value.get(string_value) == simdjson::SUCCESS) {
            claims.string_claims[name] = std::string(string_value);
            continue;
        }

        std::int64_t int_value = 0;
        if (value.get(int_value) == simdjson::SUCCESS) {
            claims.integer_claims[name] = int_value;
            continue;
        }

        bool bool_value = false;
        if (value.get(bool_value) == simdjson::SUCCESS) {
            claims.bool_claims[name] = bool_value;
            continue;
        }

        simdjson::dom::array array;
        if (value.get(array) == simdjson::SUCCESS) {
            std::vector<std::string> strings;
            for (auto item : array) {
                std::string_view item_text;
                if (item.get(item_text) == simdjson::SUCCESS) {
                    strings.emplace_back(item_text);
                }
            }
            if (!strings.empty()) {
                claims.string_array_claims[name] = std::move(strings);
            }
        }
    }
}

std::expected<VerifiedPayload, Error>
verify_token(std::string_view token_text, const JwtMiddleware::Config& cfg) {
    auto parsed = parse_compact_jwt(token_text);
    if (!parsed) return std::unexpected(parsed.error());

    simdjson::dom::parser parser;
    auto header_doc = parser.parse(parsed->header_json);
    if (header_doc.error() != simdjson::SUCCESS) {
        return std::unexpected("invalid_header_json");
    }

    const auto algorithm = get_json_string(header_doc.value(), "alg");
    if (!algorithm || *algorithm == "none" || !algorithm_allowed(cfg, *algorithm)) {
        return std::unexpected("unsupported_algorithm");
    }

    std::expected<void, Error> signature_result = {};
    if (*algorithm == "HS256") {
        signature_result = verify_hs256(*parsed, cfg.hmac_secret);
    } else if (*algorithm == "RS256") {
        signature_result = verify_rs256(*parsed, cfg.rsa_public_key_pem);
    }
    if (!signature_result) return std::unexpected(signature_result.error());

    auto payload_doc = parser.parse(parsed->payload_json);
    if (payload_doc.error() != simdjson::SUCCESS) {
        return std::unexpected("invalid_payload_json");
    }

    const auto payload = payload_doc.value();
    JwtPrincipal principal;
    read_all_claims(payload, principal.claims);

    if (auto sub = get_json_string(payload, "sub")) principal.subject = *sub;
    if (auto iss = get_json_string(payload, "iss")) principal.issuer = *iss;
    if (auto jti = get_json_string(payload, "jti")) principal.token_id = *jti;
    principal.audience = read_string_or_array(payload, "aud");
    principal.scopes = read_scopes(payload, cfg.scope_claim);

    const auto now = std::chrono::system_clock::now();

    if (auto exp = get_json_int(payload, "exp")) {
        principal.expires_at = from_unix_seconds(*exp);
        if (now - cfg.clock_skew >= *principal.expires_at) {
            return std::unexpected("token_expired");
        }
    } else if (cfg.require_expiration) {
        return std::unexpected("missing_exp");
    }

    if (cfg.validate_not_before) {
        if (auto nbf = get_json_int(payload, "nbf")) {
            principal.not_before = from_unix_seconds(*nbf);
            if (now + cfg.clock_skew < *principal.not_before) {
                return std::unexpected("token_not_yet_valid");
            }
        }
    }

    if (cfg.validate_issued_at) {
        if (auto iat = get_json_int(payload, "iat")) {
            principal.issued_at = from_unix_seconds(*iat);
            if (now + cfg.clock_skew < *principal.issued_at) {
                return std::unexpected("token_issued_in_future");
            }
        }
    }

    if (cfg.required_issuer && principal.issuer != *cfg.required_issuer) {
        return std::unexpected("issuer_mismatch");
    }

    for (const auto& required_audience : cfg.required_audiences) {
        if (!contains(principal.audience, required_audience)) {
            return std::unexpected("audience_mismatch");
        }
    }

    for (const auto& required_scope : cfg.required_scopes) {
        if (!contains(principal.scopes, required_scope)) {
            return std::unexpected("missing_scope");
        }
    }

    for (const auto& [claim, expected] : cfg.required_claims) {
        const auto actual = principal.claims.string(claim);
        if (!actual || *actual != expected) {
            return std::unexpected("claim_mismatch");
        }
    }

    return VerifiedPayload{
        .algorithm = *algorithm,
        .principal = std::move(principal),
    };
}

std::optional<std::string_view> bearer_token(const http3::Request& req,
                                             const JwtMiddleware::Config& cfg) {
    const auto header = req.header(cfg.authorization_header);
    if (!header) return std::nullopt;
    if (!header->starts_with(cfg.bearer_prefix)) return std::nullopt;
    return header->substr(cfg.bearer_prefix.size());
}

std::optional<std::string_view> cookie_token(const http3::Request& req,
                                             std::string_view cookie_name) {
    const auto header = req.header("cookie");
    if (!header) return std::nullopt;

    auto remaining = *header;
    while (!remaining.empty()) {
        const auto separator = remaining.find(';');
        auto item = remaining.substr(0, separator);
        const auto first = item.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            item.remove_prefix(first);
            const auto equals = item.find('=');
            if (equals != std::string_view::npos &&
                item.substr(0, equals) == cookie_name) {
                return item.substr(equals + 1U);
            }
        }
        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1U);
    }
    return std::nullopt;
}

void reject(http3::Response& res, const JwtMiddleware::Config& cfg) {
    res.status(cfg.unauthorized_status)
        .header("WWW-Authenticate", R"(Bearer error="invalid_token")")
        .json(cfg.unauthorized_body);
}

} // namespace

std::optional<std::string_view>
JwtClaims::string(std::string_view name) const {
    const auto it = string_claims.find(std::string(name));
    if (it == string_claims.end()) return std::nullopt;
    return it->second;
}

std::optional<std::int64_t>
JwtClaims::integer(std::string_view name) const {
    const auto it = integer_claims.find(std::string(name));
    if (it == integer_claims.end()) return std::nullopt;
    return it->second;
}

std::optional<bool>
JwtClaims::boolean(std::string_view name) const {
    const auto it = bool_claims.find(std::string(name));
    if (it == bool_claims.end()) return std::nullopt;
    return it->second;
}

const std::vector<std::string>*
JwtClaims::string_array(std::string_view name) const {
    const auto it = string_array_claims.find(std::string(name));
    if (it == string_array_claims.end()) return nullptr;
    return &it->second;
}

JwtTokenBuilder& JwtTokenBuilder::subject(std::string value) {
    subject_ = std::move(value);
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::issuer(std::string value) {
    issuer_ = std::move(value);
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::audience(std::string value) {
    audience_.push_back(std::move(value));
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::audiences(std::vector<std::string> values) {
    audience_ = std::move(values);
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::scope(std::string value) {
    scopes_.push_back(std::move(value));
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::scopes(std::vector<std::string> values) {
    scopes_ = std::move(values);
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::token_id(std::string value) {
    token_id_ = std::move(value);
    return *this;
}

JwtTokenBuilder&
JwtTokenBuilder::expires_at(std::chrono::system_clock::time_point value) {
    expires_at_ = value;
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::expires_in(std::chrono::seconds value) {
    expires_at_ = std::chrono::system_clock::now() + value;
    return *this;
}

JwtTokenBuilder&
JwtTokenBuilder::not_before(std::chrono::system_clock::time_point value) {
    not_before_ = value;
    return *this;
}

JwtTokenBuilder&
JwtTokenBuilder::issued_at(std::chrono::system_clock::time_point value) {
    issued_at_ = value;
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::issued_now() {
    issued_at_ = std::chrono::system_clock::now();
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::claim(std::string name, std::string value) {
    claims_.string_claims[std::move(name)] = std::move(value);
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::claim(std::string name, std::string_view value) {
    claims_.string_claims[std::move(name)] = std::string(value);
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::claim(std::string name, const char* value) {
    claims_.string_claims[std::move(name)] = value == nullptr ? "" : value;
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::claim(std::string name, std::int64_t value) {
    claims_.integer_claims[std::move(name)] = value;
    return *this;
}

JwtTokenBuilder& JwtTokenBuilder::claim(std::string name, bool value) {
    claims_.bool_claims[std::move(name)] = value;
    return *this;
}

JwtTokenBuilder&
JwtTokenBuilder::claim(std::string name, std::vector<std::string> value) {
    claims_.string_array_claims[std::move(name)] = std::move(value);
    return *this;
}

JwtIssuer::JwtIssuer(Config cfg)
    : cfg_(std::move(cfg)) {}

std::expected<std::string, std::string>
JwtIssuer::issue(const JwtTokenBuilder& token) const {
    return issue(cfg_, token);
}

std::expected<std::string, std::string>
JwtIssuer::issue(const Config& cfg, const JwtTokenBuilder& token) {
    const auto header = serialize_header(cfg);
    const auto payload = serialize_claims(token, cfg);
    const auto signing_input =
        base64url_encode(header) + "." + base64url_encode(payload);

    std::expected<std::string, std::string> signature =
        std::unexpected("unsupported_algorithm");

    switch (cfg.algorithm) {
    case Algorithm::HS256:
        signature = sign_hs256(signing_input, cfg.hmac_secret);
        break;
    case Algorithm::RS256:
        signature = sign_rs256(signing_input, cfg.rsa_private_key_pem);
        break;
    }

    if (!signature) return std::unexpected(signature.error());
    return signing_input + "." + base64url_encode(*signature);
}

JwtMiddleware::JwtMiddleware()
    : JwtMiddleware(Config{}) {}

JwtMiddleware::JwtMiddleware(Config cfg)
    : cfg_(std::move(cfg)) {}

std::function<websocket::HandshakeDecision(const http3::Request&)>
JwtMiddleware::websocket_authorizer() const {
    auto cfg = cfg_;
    return [cfg = std::move(cfg)](const http3::Request& request) {
        auto token = bearer_token(request, cfg);
        if (!token && cfg.websocket_cookie_name) {
            token = cookie_token(request, *cfg.websocket_cookie_name);
        }
        if (!token) {
            return websocket::HandshakeDecision::reject(
                cfg.unauthorized_status, cfg.unauthorized_body);
        }

        auto verified = verify_token(*token, cfg);
        if (!verified || verified->principal.subject.empty()) {
            return websocket::HandshakeDecision::reject(
                cfg.unauthorized_status, cfg.unauthorized_body);
        }
        return websocket::HandshakeDecision::allow(std::move(verified->principal.subject));
    };
}

void JwtMiddleware::handle(http3::Request& req,
                           http3::Response& res,
                           context::RequestContext& ctx,
                           Next next) {
    if ((cfg_.allow_options_requests && req.method() == "OPTIONS") ||
        path_allowed(req.path(), cfg_.allowlist_paths)) {
        next();
        return;
    }

    const auto token = bearer_token(req, cfg_);
    if (!token) {
        reject(res, cfg_);
        return;
    }

    auto verified = verify_token(*token, cfg_);
    if (!verified) {
        reject(res, cfg_);
        return;
    }

    ctx.set<JwtPrincipal>(std::move(verified->principal));
    next();
}

} // namespace novaboot::middleware
