#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/router/route.h"

using namespace novaboot;
using namespace novaboot::middleware;

namespace {

constexpr std::string_view kPrivateKeyPem = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDwNwfeFWL6vzLt
NDan1VvD/BJLoowQU2yvAxSe/V4sXnSCMC1OZUIwlJPmTz8r+33U0e0BIHBoU5OO
3IAlJlnlbPiwEyD5Dv2QQ8t+HoovUxtyS1RKq1uG90rlwbB7e2gWsGpS6QgXweGo
6D0erjeNw+N7aL7UKkL9FAwlACNs24dAdfiY4T13Jut4M/ywEpEEg+NN1Tq9bzJ6
jNtxxQmclabvolNT3TL5Wz6uAHJXcT/NhQB4w8nCHDvDe0tGDcV+31jwEykQZL56
uTVbuz8QLAjEqj/vLt7vSgmqD51yAVCTSEPbFmM6hy7lTSIXaCBYGOVhPFwcB+6q
4QDJyfXxAgMBAAECggEBAMrl8KNJdT1O2nk5LSE+OjCkbd+tAJmZnaeF9pkEcdMQ
RWDhyKL7LJlUJkjWjwlbd8GXP5VADTJRxcVZwnOenTgIf1ZaEfPNEagVW4C/0wB+
NiSoNB6N9tVdc8n1fFgQuK7RfD1j0A4hGbZN07JifVIlL7RBmU4kFEkn8coe/80l
TWJLH/PRzrLtvsL9y2GOJ57C6aUj92yDDeV9/7HJKG1ahO+CMTwDogOH8TfGpULF
W/K8WRg/2OcrXS81AXTSEgVRaeMs0AcOfRIWxCMEGQew/MzB/8UWu2UxzoZXoMyS
GpvWmEYOG19aNHmJRTl+qUiNFjLJkXRfIE8///knInECgYEA/exwB+S8PooEpUWI
Uccc4Ui6NH/4KUlUENNIFC5s2993yESEHJZCSMk7CQnYsySU3/jgaxR//jmtXD3O
dgeCVQP8eTbVrcRDk7kiXZ+jTUKMEEMx92xU8k9NChLYmTOXiIDfNj2i8dKYPFg5
teLi2T1QpzxQWrQsRrQ02pBrAocCgYEA8i3lQvieHKscYkZxXjfsJ4G6XT6Htrdp
DeT/m3D1KizoXJiyliB+avWkp6q9yHD8KRyedhuncNusxG21opsKAteyD2jtCRtp
WOz4BfRA9JQ3Ah3qsZYm7l04yWO5SV8foAOINDyee3ZfldaLKznPy46gg40M/LK3
83HaKJYpyccCgYBRntCcGPBRgffgUCtzbfdgiwofmgrg6os1JVUD97BSNNDB9RQq
RDpmPxjU3O9lFhssq0kn4l0KEOwivFNDKULBpRhgtjmVM4MtVtMvGQNa8EspZxAG
ojHj+Y2f8VLiW+0XmaUmQEXCSZlZvFpAlv+oKMdmGkMSBNw+lepvXwCmvQKBgAWl
fErrcQRKGQdRjSMdOqxRIf86jf98lz5zsGH3aD/rfB9sj/1gFJJ517TxDiu2Nqre
t/MZAfZnqMeLp0h7ROh8DvK9B4nG0dX48G9zGnCZZA7wk6BAV/gGxGQtpwxG433f
lzVglHFceS4NKoF/f5DHaoamqz7ZC+DpLqTzm1XRAoGBANWC5RpmmvFADhw85ZnF
92mFrn4ZAdVo6EuCMNWJkwuvT9gg7shZLRZ+TPT8C//eiD6wqxeACztj29hQdRQb
7S/cOuc4cQJHC1hBc8zOvnTrRIVhE3cH6U2AqSzGIj3lgmyrc8qA/a3UAXmeUEYK
bHMYk+suDGLYfs2bgk+uiOw/
-----END PRIVATE KEY-----)";

constexpr std::string_view kPublicKeyPem = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA8DcH3hVi+r8y7TQ2p9Vb
w/wSS6KMEFNsrwMUnv1eLF50gjAtTmVCMJST5k8/K/t91NHtASBwaFOTjtyAJSZZ
5Wz4sBMg+Q79kEPLfh6KL1MbcktUSqtbhvdK5cGwe3toFrBqUukIF8HhqOg9Hq43
jcPje2i+1CpC/RQMJQAjbNuHQHX4mOE9dybreDP8sBKRBIPjTdU6vW8yeozbccUJ
nJWm76JTU90y+Vs+rgByV3E/zYUAeMPJwhw7w3tLRg3Fft9Y8BMpEGS+erk1W7s/
ECwIxKo/7y7e70oJqg+dcgFQk0hD2xZjOocu5U0iF2ggWBjlYTxcHAfuquEAycn1
8QIDAQAB
-----END PUBLIC KEY-----)";

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

std::string hmac_sha256(std::string_view data, std::string_view secret) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(),
         secret.data(),
         static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         data.size(),
         digest.data(),
         &digest_len);
    return std::string(reinterpret_cast<const char*>(digest.data()), digest_len);
}

std::string rsa_sha256(std::string_view data) {
    auto* bio = BIO_new_mem_buf(kPrivateKeyPem.data(),
                                static_cast<int>(kPrivateKeyPem.size()));
    auto* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    auto* ctx = EVP_MD_CTX_new();

    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key);
    EVP_DigestSignUpdate(ctx, data.data(), data.size());

    std::size_t sig_len = 0;
    EVP_DigestSignFinal(ctx, nullptr, &sig_len);
    std::string signature(sig_len, '\0');
    EVP_DigestSignFinal(
        ctx,
        reinterpret_cast<unsigned char*>(signature.data()),
        &sig_len);
    signature.resize(sig_len);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    BIO_free(bio);
    return signature;
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string token(std::string_view algorithm,
                  std::string_view payload,
                  std::string_view secret = "secret") {
    const std::string header =
        std::string(R"({"alg":")") + std::string(algorithm) + R"(","typ":"JWT"})";
    const auto signing_input =
        base64url_encode(header) + "." + base64url_encode(payload);

    std::string signature;
    if (algorithm == "HS256") {
        signature = hmac_sha256(signing_input, secret);
    } else {
        signature = rsa_sha256(signing_input);
    }

    return signing_input + "." + base64url_encode(signature);
}

JwtMiddleware::Config hs_config() {
    JwtMiddleware::Config cfg;
    cfg.allowed_algorithms = {JwtMiddleware::Algorithm::HS256};
    cfg.hmac_secret = "secret";
    cfg.required_issuer = "novaboot";
    cfg.required_audiences = {"api"};
    cfg.required_scopes = {"read"};
    return cfg;
}

void run_one(Middleware& mw,
             http3::Request& req,
             http3::Response& res,
             context::RequestContext& ctx,
             router::Handler handler) {
    Pipeline pipeline;
    pipeline.add(std::shared_ptr<Middleware>(&mw, [](auto*) {}));
    pipeline.execute(req, res, ctx, handler);
}

std::string valid_payload() {
    return std::string(R"({"sub":"user-123","iss":"novaboot","aud":["api","mobile"],)")
        + R"("scope":"read write","role":"admin","exp":)"
        + std::to_string(unix_now() + 3600)
        + R"(,"iat":)"
        + std::to_string(unix_now() - 10)
        + "}";
}

} // namespace

TEST(JwtMiddlewareTest, AllowlistedPathSkipsAuthentication) {
    auto cfg = hs_config();
    cfg.allowlist_paths = {"/health", "/public/*"};
    JwtMiddleware mw(cfg);

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/public/docs");

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(204);
        });

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 204);
    EXPECT_EQ(ctx.get<JwtPrincipal>(), nullptr);
}

TEST(JwtMiddlewareTest, MissingTokenIsRejected) {
    JwtMiddleware mw(hs_config());
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            called = true;
        });

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 401);
    EXPECT_TRUE(res.headers().get("WWW-Authenticate").has_value());
}

TEST(JwtMiddlewareTest, ValidHs256TokenStoresPrincipal) {
    JwtMiddleware mw(hs_config());
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    req.headers().set("authorization",
                      "Bearer " + token("HS256", valid_payload()));

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(200);
        });

    ASSERT_TRUE(called);
    ASSERT_NE(ctx.get<JwtPrincipal>(), nullptr);
    const auto& principal = *ctx.get<JwtPrincipal>();
    EXPECT_EQ(principal.subject, "user-123");
    EXPECT_EQ(principal.issuer, "novaboot");
    EXPECT_EQ(principal.scopes.size(), 2);
    EXPECT_EQ(principal.claims.string("role").value_or(""), "admin");
}

TEST(JwtMiddlewareTest, CreatesSelfContainedWebSocketJwtAuthorizer) {
    auto cfg = hs_config();
    JwtMiddleware middleware(cfg);
    const auto authorize = middleware.websocket_authorizer();

    http3::Request valid_request;
    valid_request.headers().set("authorization",
                                "Bearer " + token("HS256", valid_payload()));
    const auto allowed = authorize(valid_request);
    EXPECT_TRUE(allowed.accepted);
    EXPECT_EQ(allowed.principal, "user-123");

    http3::Request missing_token;
    const auto rejected = authorize(missing_token);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.rejection_status, 401);
    EXPECT_EQ(rejected.rejection_body, cfg.unauthorized_body);
}

TEST(JwtMiddlewareTest, WebSocketAuthorizerCanUseAnOptInCookieToken) {
    auto cfg = hs_config();
    cfg.websocket_cookie_name = "nova_access";
    JwtMiddleware middleware(cfg);
    const auto authorize = middleware.websocket_authorizer();

    http3::Request request;
    request.headers().set("cookie", "theme=dark; nova_access=" +
                                      token("HS256", valid_payload()));
    const auto allowed = authorize(request);

    EXPECT_TRUE(allowed.accepted);
    EXPECT_EQ(allowed.principal, "user-123");
}

TEST(JwtMiddlewareTest, InvalidSignatureIsRejected) {
    JwtMiddleware mw(hs_config());
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    req.headers().set("authorization",
                      "Bearer " + token("HS256", valid_payload(), "wrong"));

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            called = true;
        });

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 401);
}

TEST(JwtMiddlewareTest, ExpiredTokenIsRejected) {
    JwtMiddleware mw(hs_config());
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");

    const auto payload =
        std::string(R"({"sub":"user-123","iss":"novaboot","aud":"api",)")
        + R"("scope":"read","exp":)"
        + std::to_string(unix_now() - 120)
        + "}";
    req.headers().set("authorization", "Bearer " + token("HS256", payload));

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            called = true;
        });

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 401);
}

TEST(JwtMiddlewareTest, RequiredScopeIsEnforced) {
    auto cfg = hs_config();
    cfg.required_scopes = {"admin"};
    JwtMiddleware mw(cfg);

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    req.headers().set("authorization",
                      "Bearer " + token("HS256", valid_payload()));

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            called = true;
        });

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 401);
}

TEST(JwtMiddlewareTest, Rs256TokenIsAccepted) {
    JwtMiddleware::Config cfg;
    cfg.allowed_algorithms = {JwtMiddleware::Algorithm::RS256};
    cfg.rsa_public_key_pem = std::string(kPublicKeyPem);
    cfg.required_issuer = "novaboot";
    cfg.required_audiences = {"api"};

    JwtMiddleware mw(cfg);
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    req.headers().set("authorization",
                      "Bearer " + token("RS256", valid_payload()));

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(200);
        });

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
    ASSERT_NE(ctx.get<JwtPrincipal>(), nullptr);
    EXPECT_EQ(ctx.get<JwtPrincipal>()->subject, "user-123");
}

TEST(JwtIssuerTest, IssuesHs256TokenAcceptedByMiddleware) {
    JwtIssuer issuer(JwtIssuer::Config{
        .algorithm = JwtIssuer::Algorithm::HS256,
        .hmac_secret = "secret",
        .rsa_private_key_pem = "",
        .key_id = "",
        .default_ttl = std::chrono::hours{1},
    });

    JwtTokenBuilder builder;
    builder.subject("issued-user")
        .issuer("novaboot")
        .audience("api")
        .scopes({"read", "write"})
        .token_id("token-1")
        .claim("role", "admin")
        .claim("tenant_id", static_cast<std::int64_t>(42))
        .claim("enabled", true)
        .claim("groups", std::vector<std::string>{"dev", "ops"});

    auto issued = issuer.issue(builder);
    ASSERT_TRUE(issued.has_value()) << issued.error();

    JwtMiddleware mw(hs_config());
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    req.headers().set("authorization", "Bearer " + *issued);

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(200);
        });

    ASSERT_TRUE(called);
    ASSERT_NE(ctx.get<JwtPrincipal>(), nullptr);
    const auto& principal = *ctx.get<JwtPrincipal>();
    EXPECT_EQ(principal.subject, "issued-user");
    EXPECT_EQ(principal.token_id, "token-1");
    EXPECT_EQ(principal.claims.string("role").value_or(""), "admin");
    EXPECT_EQ(principal.claims.integer("tenant_id").value_or(0), 42);
    EXPECT_EQ(principal.claims.boolean("enabled").value_or(false), true);
    ASSERT_NE(principal.claims.string_array("groups"), nullptr);
    EXPECT_EQ(principal.claims.string_array("groups")->size(), 2);
}

TEST(JwtIssuerTest, IssuesRs256TokenAcceptedByMiddleware) {
    JwtIssuer issuer(JwtIssuer::Config{
        .algorithm = JwtIssuer::Algorithm::RS256,
        .hmac_secret = "",
        .rsa_private_key_pem = std::string(kPrivateKeyPem),
        .key_id = "",
        .default_ttl = std::chrono::hours{1},
    });

    JwtTokenBuilder builder;
    builder.subject("issued-rsa-user")
        .issuer("novaboot")
        .audiences({"api", "mobile"})
        .scope("read");

    auto issued = issuer.issue(builder);
    ASSERT_TRUE(issued.has_value()) << issued.error();

    JwtMiddleware::Config cfg;
    cfg.allowed_algorithms = {JwtMiddleware::Algorithm::RS256};
    cfg.rsa_public_key_pem = std::string(kPublicKeyPem);
    cfg.required_issuer = "novaboot";
    cfg.required_audiences = {"api"};
    cfg.required_scopes = {"read"};

    JwtMiddleware mw(cfg);
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("GET");
    req.set_path("/api/users");
    req.headers().set("authorization", "Bearer " + *issued);

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(200);
        });

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 200);
    ASSERT_NE(ctx.get<JwtPrincipal>(), nullptr);
    EXPECT_EQ(ctx.get<JwtPrincipal>()->subject, "issued-rsa-user");
}

TEST(JwtIssuerTest, MissingSigningKeyReturnsError) {
    JwtTokenBuilder builder;
    builder.subject("no-key");

    auto issued = JwtIssuer::issue(
        JwtIssuer::Config{
            .algorithm = JwtIssuer::Algorithm::HS256,
            .hmac_secret = "",
            .rsa_private_key_pem = "",
            .key_id = "",
        },
        builder);

    ASSERT_FALSE(issued.has_value());
    EXPECT_EQ(issued.error(), "missing_hmac_secret");
}
