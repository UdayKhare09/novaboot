#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <zlib.h>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/middleware/body_size_limit_middleware.h"
#include "novaboot/middleware/compression_middleware.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/middleware/security_headers_middleware.h"
#include "novaboot/router/route.h"

using namespace novaboot;
using namespace novaboot::middleware;

namespace {

void run_one(Middleware& mw,
             http3::Request& req,
             http3::Response& res,
             context::RequestContext& ctx,
             router::Handler handler) {
    Pipeline pipeline;
    pipeline.add(std::shared_ptr<Middleware>(&mw, [](auto*) {}));
    pipeline.execute(req, res, ctx, handler);
}

std::string gunzip(std::string_view compressed) {
    z_stream stream{};
    EXPECT_EQ(inflateInit2(&stream, MAX_WBITS + 16), Z_OK);

    std::string output;
    output.resize(8192);

    stream.next_in = reinterpret_cast<Bytef*>(
        const_cast<char*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int result = inflate(&stream, Z_FINISH);
    EXPECT_EQ(result, Z_STREAM_END);
    output.resize(stream.total_out);
    inflateEnd(&stream);
    return output;
}

} // namespace

TEST(SecurityHeadersMiddlewareTest, AddsDefaultHeadersAfterHandler) {
    SecurityHeadersMiddleware mw;
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_scheme("https");

    run_one(mw, req, res, ctx,
        [](http3::Request&, http3::Response& r, context::RequestContext&) {
            r.status(200).body("OK");
        });

    EXPECT_EQ(res.headers().get("Strict-Transport-Security").value_or(""), "max-age=31536000; includeSubDomains");
    EXPECT_EQ(res.headers().get("X-Content-Type-Options").value_or(""), "nosniff");
    EXPECT_EQ(res.headers().get("X-Frame-Options").value_or(""), "DENY");
    EXPECT_EQ(res.headers().get("X-XSS-Protection").value_or(""), "0");
    EXPECT_EQ(res.headers().get("Referrer-Policy").value_or(""), "no-referrer");
    EXPECT_TRUE(res.headers().get("Permissions-Policy").has_value());
}

TEST(SecurityHeadersMiddlewareTest, DoesNotEmitHstsOnAnInsecureOrUnknownScheme) {
    SecurityHeadersMiddleware mw;
    http3::Request req;
    req.set_scheme("http");
    http3::Response res;
    context::RequestContext ctx;

    run_one(mw, req, res, ctx,
        [](http3::Request&, http3::Response&, context::RequestContext&) {});

    EXPECT_FALSE(res.headers().get("Strict-Transport-Security").has_value());
}

TEST(SecurityHeadersMiddlewareTest, RejectsUnsafeConfiguredHeaderValues) {
    auto invalid_value = SecurityHeadersMiddleware::Config{};
    invalid_value.content_security_policy = "default-src 'self'\r\nX-Evil: true";
    EXPECT_THROW(SecurityHeadersMiddleware(std::move(invalid_value)), std::invalid_argument);

    auto invalid_name = SecurityHeadersMiddleware::Config{};
    invalid_name.custom_headers.emplace("X-Good\nBad", "value");
    EXPECT_THROW(SecurityHeadersMiddleware(std::move(invalid_name)), std::invalid_argument);
}

TEST(SecurityHeadersMiddlewareTest, DoesNotOverwriteHandlerHeader) {
    SecurityHeadersMiddleware mw;
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;

    run_one(mw, req, res, ctx,
        [](http3::Request&, http3::Response& r, context::RequestContext&) {
            r.header("X-Frame-Options", "SAMEORIGIN");
        });

    EXPECT_EQ(res.headers().get("X-Frame-Options").value_or(""), "SAMEORIGIN");
}

TEST(BodySizeLimitMiddlewareTest, RejectsLargeBody) {
    BodySizeLimitMiddleware mw({.max_body_bytes = 4});
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("POST");
    req.set_path("/api/upload");
    req.append_body(reinterpret_cast<const std::uint8_t*>("too-large"), 9);

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            called = true;
        });

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 413);
    EXPECT_EQ(res.headers().get("content-type").value_or(""), "application/json");
}

TEST(BodySizeLimitMiddlewareTest, UsesDeclaredContentLengthWhenPresent) {
    BodySizeLimitMiddleware mw({.max_body_bytes = 4});
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_method("POST");
    req.set_path("/api/upload");
    req.headers().set("content-length", "5");

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response&, context::RequestContext&) {
            called = true;
        });

    EXPECT_FALSE(called);
    EXPECT_EQ(res.status_code(), 413);
}

TEST(BodySizeLimitMiddlewareTest, AllowlistedPathBypassesLimit) {
    BodySizeLimitMiddleware mw({
        .max_body_bytes = 4,
        .allowlist_paths = {"/uploads/*"},
    });
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.set_path("/uploads/raw");
    req.append_body(reinterpret_cast<const std::uint8_t*>("too-large"), 9);

    bool called = false;
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            called = true;
            r.status(201);
        });

    EXPECT_TRUE(called);
    EXPECT_EQ(res.status_code(), 201);
}

TEST(CompressionMiddlewareTest, GzipsEligibleResponse) {
    CompressionMiddleware mw({.min_size_bytes = 16});
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.headers().set("accept-encoding", "br, gzip");

    const std::string body(512, 'x');
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            r.header("Content-Type", "application/json")
             .header("Content-Length", std::to_string(body.size()))
             .body(body);
        });

    EXPECT_EQ(res.headers().get("Content-Encoding").value_or(""), "gzip");
    EXPECT_EQ(res.headers().get("Vary").value_or(""), "Accept-Encoding");
    EXPECT_FALSE(res.headers().get("Content-Length").has_value());
    EXPECT_LT(res.body_size(), body.size());
    EXPECT_EQ(gunzip(res.body_data()), body);
}

TEST(CompressionMiddlewareTest, SkipsWhenClientDoesNotAcceptGzip) {
    CompressionMiddleware mw({.min_size_bytes = 16});
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;

    const std::string body(512, 'x');
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            r.header("Content-Type", "application/json").body(body);
        });

    EXPECT_FALSE(res.headers().get("Content-Encoding").has_value());
    EXPECT_EQ(res.body_data(), body);
}

TEST(CompressionMiddlewareTest, SkipsAlreadyEncodedResponse) {
    CompressionMiddleware mw({.min_size_bytes = 16});
    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;
    req.headers().set("accept-encoding", "gzip");

    const std::string body(512, 'x');
    run_one(mw, req, res, ctx,
        [&](http3::Request&, http3::Response& r, context::RequestContext&) {
            r.header("Content-Type", "application/json")
             .header("Content-Encoding", "br")
             .body(body);
        });

    EXPECT_EQ(res.headers().get("Content-Encoding").value_or(""), "br");
    EXPECT_EQ(res.body_data(), body);
}
