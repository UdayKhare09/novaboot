#include <gtest/gtest.h>

#include "novaboot/context/request_context.h"
#include "novaboot/middleware/trusted_forwarded_headers_middleware.h"

namespace {

TEST(TrustedForwardedHeadersMiddlewareTest, RequiresExplicitTrustAcknowledgement) {
    EXPECT_THROW(novaboot::middleware::TrustedForwardedHeadersMiddleware(),
                 std::invalid_argument);
}

TEST(TrustedForwardedHeadersMiddlewareTest, AppliesFirstForwardedProtoAndHost) {
    novaboot::middleware::TrustedForwardedHeadersMiddleware middleware({
        .trust_all_direct_peers = true,
    });
    novaboot::http3::Request request;
    request.set_scheme("http");
    request.set_authority("internal:8080");
    request.headers().set("forwarded",
                          "for=192.0.2.10;Proto=https;HOST=api.example.test, for=198.51.100.2;proto=http");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool called = false;

    middleware.handle(request, response, context, [&] { called = true; });

    EXPECT_TRUE(called);
    EXPECT_EQ(request.scheme(), "https");
    EXPECT_EQ(request.authority(), "api.example.test");
    EXPECT_EQ(request.client_address(), "192.0.2.10");
}

TEST(TrustedForwardedHeadersMiddlewareTest, AppliesHeadersOnlyFromTrustedPeerCidr) {
    novaboot::middleware::TrustedForwardedHeadersMiddleware middleware({
        .trusted_peer_cidrs = {"10.42.0.0/16", "2001:db8:42::/48"},
    });
    novaboot::http3::Request request;
    request.set_peer_address("10.42.9.7");
    request.set_scheme("http");
    request.set_authority("internal:8080");
    request.headers().set("forwarded", "for=198.51.100.7;proto=https;host=api.example.test");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;

    middleware.handle(request, response, context, [] {});
    EXPECT_EQ(request.scheme(), "https");
    EXPECT_EQ(request.authority(), "api.example.test");
    EXPECT_EQ(request.client_address(), "198.51.100.7");

    request.set_peer_address("203.0.113.9");
    request.set_scheme("http");
    request.set_authority("internal:8080");
    middleware.handle(request, response, context, [] {});
    EXPECT_EQ(request.scheme(), "http");
    EXPECT_EQ(request.authority(), "internal:8080");
    EXPECT_EQ(request.client_address(), "203.0.113.9");
}

TEST(TrustedForwardedHeadersMiddlewareTest, IgnoresMalformedForwardedValue) {
    novaboot::middleware::TrustedForwardedHeadersMiddleware middleware({
        .trust_all_direct_peers = true,
    });
    novaboot::http3::Request request;
    request.set_scheme("http");
    request.set_authority("internal:8080");
    request.headers().set("forwarded", "proto=https;host=api.example.test\r\nX-Evil: yes");
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;

    middleware.handle(request, response, context, [] {});

    EXPECT_EQ(request.scheme(), "https");
    EXPECT_EQ(request.authority(), "internal:8080");
}

} // namespace
