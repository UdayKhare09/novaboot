#include <gtest/gtest.h>
#include "novaboot/core/http_drain.h"

TEST(HttpDrainGateTest, RejectsNewRequestsAfterShutdownBegins) {
    novaboot::core::HttpDrainGate gate;
    novaboot::http3::Request request;
    novaboot::http3::Response response;
    novaboot::context::RequestContext context;
    bool invoked = false;
    gate.handle(request, response, context, [&] { invoked = true; });
    EXPECT_TRUE(invoked);
    EXPECT_TRUE(gate.drained());
    gate.stop_accepting();
    response = {};
    invoked = false;
    gate.handle(request, response, context, [&] { invoked = true; });
    EXPECT_FALSE(invoked);
    EXPECT_EQ(response.status_code(), 503);
}
