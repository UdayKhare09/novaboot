#include <gtest/gtest.h>
#include "novaboot/core/shutdown.h"

TEST(ShutdownCoordinatorTest, StopsAcceptingAndCompletesDrainedParticipants) {
    novaboot::core::ShutdownCoordinator coordinator;
    bool stopped = false;
    coordinator.add({.name = "http", .stop_accepting = [&] { stopped = true; },
                     .drained = [&] { return stopped; }, .force_close = {}});
    const auto result = coordinator.drain_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(stopped);
    EXPECT_TRUE(result.drained);
}

TEST(ShutdownCoordinatorTest, ForcesParticipantsPastDeadline) {
    novaboot::core::ShutdownCoordinator coordinator;
    bool forced = false;
    coordinator.add({.name = "websocket", .stop_accepting = {}, .drained = [] { return false; },
                     .force_close = [&] { forced = true; }});
    const auto result = coordinator.drain_for(std::chrono::milliseconds(0));
    EXPECT_FALSE(result.drained);
    EXPECT_TRUE(forced);
    ASSERT_EQ(result.forced_participants.size(), 1U);
    EXPECT_EQ(result.forced_participants[0], "websocket");
}
