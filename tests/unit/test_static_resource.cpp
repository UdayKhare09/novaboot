#include <gtest/gtest.h>

#include "novaboot/http/static_resource.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

class StaticResourceTest : public ::testing::Test {
protected:
    std::filesystem::path root_;

    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
            ("novaboot-static-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_);
        std::ofstream file(root_ / "hello.txt", std::ios::binary);
        file << "hello static world";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] novaboot::http3::Request request(std::string_view path) const {
        novaboot::http3::Request request;
        request.set_method("GET");
        request.set_path(path);
        return request;
    }
};

TEST_F(StaticResourceTest, ServesSafeFilesWithCacheValidator) {
    auto request = this->request("/hello.txt");
    novaboot::http3::Response response;

    ASSERT_TRUE(novaboot::http::serve_static_resource(root_, request, response));
    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(response.body_data(), "hello static world");
    EXPECT_EQ(response.headers().get("content-length"), "18");
    ASSERT_TRUE(response.headers().get("etag"));
    EXPECT_EQ(response.headers().get("accept-ranges"), "bytes");
}

TEST_F(StaticResourceTest, SupportsConditionalHeadAndSingleRangeResponses) {
    auto initial = this->request("/hello.txt");
    novaboot::http3::Response initial_response;
    ASSERT_TRUE(novaboot::http::serve_static_resource(root_, initial, initial_response));
    const auto etag = *initial_response.headers().get("etag");

    auto conditional = this->request("/hello.txt");
    conditional.headers().set("if-none-match", etag);
    novaboot::http3::Response not_modified;
    ASSERT_TRUE(novaboot::http::serve_static_resource(root_, conditional, not_modified));
    EXPECT_EQ(not_modified.status_code(), 304);
    EXPECT_TRUE(not_modified.body_data().empty());

    auto range = this->request("/hello.txt");
    range.headers().set("range", "bytes=6-11");
    novaboot::http3::Response partial;
    ASSERT_TRUE(novaboot::http::serve_static_resource(root_, range, partial));
    EXPECT_EQ(partial.status_code(), 206);
    EXPECT_EQ(partial.body_data(), "static");
    EXPECT_EQ(partial.headers().get("content-range"), "bytes 6-11/18");
    EXPECT_EQ(partial.headers().get("content-length"), "6");

    novaboot::http3::Response head;
    ASSERT_TRUE(novaboot::http::serve_static_resource(root_, initial, head, true));
    EXPECT_EQ(head.status_code(), 200);
    EXPECT_TRUE(head.body_data().empty());
    EXPECT_EQ(head.headers().get("content-length"), "18");
}

TEST_F(StaticResourceTest, RejectsTraversalAndReportsInvalidRange) {
    auto traversal = this->request("/../hello.txt");
    novaboot::http3::Response traversal_response;
    EXPECT_FALSE(novaboot::http::serve_static_resource(root_, traversal, traversal_response));

    auto range = this->request("/hello.txt");
    range.headers().set("range", "bytes=100-200");
    novaboot::http3::Response response;
    ASSERT_TRUE(novaboot::http::serve_static_resource(root_, range, response));
    EXPECT_EQ(response.status_code(), 416);
    EXPECT_EQ(response.headers().get("content-range"), "bytes */18");
}

} // namespace
