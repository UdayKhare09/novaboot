#include <gtest/gtest.h>
#include "novaboot/http3/header_map.h"

using namespace novaboot::http3;

TEST(HeaderMapTest, BasicOperations) {
    HeaderMap headers;
    EXPECT_TRUE(headers.empty());
    EXPECT_EQ(headers.size(), 0);

    headers.add("Content-Type", "application/json");
    EXPECT_FALSE(headers.empty());
    EXPECT_EQ(headers.size(), 1);
    EXPECT_TRUE(headers.has("content-type")); // case-insensitive check
    EXPECT_EQ(headers.get("content-type").value_or(""), "application/json");
}

TEST(HeaderMapTest, CaseInsensitivity) {
    HeaderMap headers;
    headers.add("X-Test-Header", "Value1");

    EXPECT_TRUE(headers.has("x-test-header"));
    EXPECT_TRUE(headers.has("X-TEST-HEADER"));
    EXPECT_TRUE(headers.has("X-Test-Header"));

    EXPECT_EQ(headers.get("x-test-header").value(), "Value1");
    EXPECT_EQ(headers.get("X-TEST-HEADER").value(), "Value1");
}

TEST(HeaderMapTest, SetReplacesExisting) {
    HeaderMap headers;
    headers.set("Host", "localhost:8080");
    EXPECT_EQ(headers.get("host").value(), "localhost:8080");

    headers.set("host", "127.0.0.1:4433");
    EXPECT_EQ(headers.size(), 1);
    EXPECT_EQ(headers.get("Host").value(), "127.0.0.1:4433");
}

TEST(HeaderMapTest, MultiValueSupport) {
    HeaderMap headers;
    headers.add("Accept", "text/html");
    headers.add("accept", "application/xhtml+xml");

    EXPECT_EQ(headers.size(), 2);
    auto values = headers.get_all("ACCEPT");
    ASSERT_EQ(values.size(), 2);
    EXPECT_EQ(values[0], "text/html");
    EXPECT_EQ(values[1], "application/xhtml+xml");
}

TEST(HeaderMapTest, Remove) {
    HeaderMap headers;
    headers.add("Cookie", "session=123");
    headers.add("cookie", "theme=dark");
    EXPECT_EQ(headers.size(), 2);

    headers.remove("COOKIE");
    EXPECT_EQ(headers.size(), 0);
    EXPECT_FALSE(headers.has("cookie"));
}
