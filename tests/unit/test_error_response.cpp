#include <gtest/gtest.h>

#include "novaboot/core/error_response.h"

namespace {

TEST(ErrorResponseTest, HidesExceptionMessageByDefault) {
    novaboot::http3::Response response;
    const std::runtime_error error("database password=do-not-expose");

    novaboot::core::write_unhandled_error(response, error);

    EXPECT_EQ(response.status_code(), 500);
    EXPECT_EQ(response.body_data(), R"({"error":"Internal Server Error"})");
    EXPECT_EQ(response.body_data().find("password"), std::string_view::npos);
}

TEST(ErrorResponseTest, ExplicitDevelopmentDetailsAreJsonEscaped) {
    novaboot::http3::Response response;
    const std::runtime_error error("failure: \"quoted\"\nnext line");

    novaboot::core::write_unhandled_error(response, error, true);

    EXPECT_EQ(response.status_code(), 500);
    EXPECT_EQ(response.body_data(),
              R"({"error":"Internal Server Error","message":"failure: \"quoted\"\nnext line"})");
}

} // namespace
