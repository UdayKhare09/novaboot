#include <filesystem>
#include <gtest/gtest.h>

#include "novaboot/core/server.h"
#include "novaboot/testing/live_server.h"

namespace {

std::string locate_file(std::string name) {
    std::filesystem::path path = std::move(name);
    for (int depth = 0; depth < 4; ++depth) {
        if (std::filesystem::exists(path)) return path.string();
        path = ".." / path;
    }
    return {};
}

TEST(LiveServerTest, StartsAndStopsWithDeterministicReadiness) {
    auto server = novaboot::Server::create()
        .bind("127.0.0.1", 4441)
        .tls(locate_file("cert.pem"), locate_file("key.pem"))
        .workers(1)
        .build();
    server->route("/test").get([](auto&, auto& response, auto&) {
        response.text("ok");
    });

    novaboot::testing::LiveServer live(std::move(server));
    EXPECT_TRUE(live.server().is_ready());
    live.stop();
    EXPECT_FALSE(live.server().is_ready());
}

} // namespace
