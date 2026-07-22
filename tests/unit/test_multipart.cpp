#include <gtest/gtest.h>

#include "novaboot/http/multipart.h"

#include <filesystem>

namespace {

novaboot::http3::Request multipart_request(std::string_view body) {
    novaboot::http3::Request request;
    request.set_method("POST");
    request.headers().set("content-type", "multipart/form-data; boundary=NovaBoundary");
    request.append_body(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    return request;
}

constexpr std::string_view kBody =
    "--NovaBoundary\r\n"
    "Content-Disposition: form-data; name=title\r\n\r\n"
    "NovaBoot\r\n"
    "--NovaBoundary\r\n"
    "Content-Disposition: form-data; name=attachment; filename=notes.txt\r\n"
    "Content-Type: text/plain\r\n\r\n"
    "hello upload\r\n"
    "--NovaBoundary--\r\n";

TEST(MultipartTest, ParsesFieldsAndInMemoryFiles) {
    const auto parsed = novaboot::http::parse_multipart(multipart_request(kBody));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    ASSERT_TRUE(parsed->field("title"));
    EXPECT_EQ(*parsed->field("title"), "NovaBoot");
    ASSERT_EQ(parsed->files().size(), 1U);
    const auto& file = parsed->files().front();
    EXPECT_EQ(file.field_name(), "attachment");
    EXPECT_EQ(file.filename(), "notes.txt");
    EXPECT_EQ(file.content_type(), "text/plain");
    EXPECT_TRUE(file.in_memory());
    EXPECT_EQ(file.data(), "hello upload");
}

TEST(MultipartTest, SpillsLargeFilesAndRemovesTheTemporaryFileWithFormLifetime) {
    std::filesystem::path temporary_path;
    {
        novaboot::http::MultipartLimits limits;
        limits.max_in_memory_file_bytes = 4;
        const auto parsed = novaboot::http::parse_multipart(multipart_request(kBody), limits);
        ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
        const auto& file = parsed->files().front();
        EXPECT_FALSE(file.in_memory());
        temporary_path = file.temporary_path();
        EXPECT_TRUE(std::filesystem::exists(temporary_path));
    }
    EXPECT_FALSE(std::filesystem::exists(temporary_path));
}

TEST(MultipartTest, RejectsInvalidBodiesAndConfiguredLimitViolations) {
    const auto malformed = novaboot::http::parse_multipart(multipart_request("not multipart"));
    ASSERT_FALSE(malformed.has_value());
    EXPECT_NE(malformed.error().message.find("boundary"), std::string::npos);

    novaboot::http::MultipartLimits limits;
    limits.max_total_bytes = 5;
    const auto too_large = novaboot::http::parse_multipart(multipart_request(kBody), limits);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_NE(too_large.error().message.find("size limit"), std::string::npos);
}

} // namespace
