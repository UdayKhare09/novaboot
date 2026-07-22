#include <gtest/gtest.h>

#include <stdexcept>

#include "novaboot/async/task.h"
#include "novaboot/async/cancellation.h"

namespace {

novaboot::async::Task<int> immediate_value(int value) {
    co_return value;
}

novaboot::async::Task<int> nested_value(int value) {
    co_return (co_await immediate_value(value)) + 1;
}

std::coroutine_handle<> suspended_child_handle;

novaboot::async::Task<int> suspended_value() {
    co_await novaboot::async::EventLoopSuspend{&suspended_child_handle};
    co_return 41;
}

novaboot::async::Task<int> nested_suspended_value() {
    co_return (co_await suspended_value()) + 1;
}

novaboot::async::Task<int> failing_value() {
    throw std::runtime_error("expected task failure");
    co_return 0;
}

novaboot::async::Task<int> nested_failure() {
    co_return co_await failing_value();
}

} // namespace

TEST(AsyncTaskTest, NestedCompletionDoesNotDestroyAStillRunningChild) {
    auto task = nested_value(41);
    ASSERT_TRUE(task.is_ready());
    EXPECT_EQ(task.await_resume(), 42);
}

TEST(AsyncTaskTest, NestedSuspendedCompletionResumesOnlyAfterChildFinalSuspend) {
    suspended_child_handle = nullptr;
    auto task = nested_suspended_value();
    ASSERT_FALSE(task.is_ready());
    ASSERT_TRUE(suspended_child_handle);

    suspended_child_handle.resume();

    ASSERT_TRUE(task.is_ready());
    EXPECT_EQ(task.await_resume(), 42);
}

TEST(AsyncTaskTest, NestedExceptionsPropagateAfterChildFinalSuspension) {
    auto task = nested_failure();
    ASSERT_TRUE(task.is_ready());
    EXPECT_THROW((void)task.await_resume(), std::runtime_error);
}

TEST(AsyncCancellationTest, CallbackRunsOnceAndRegistrationCanBeRemoved) {
    novaboot::async::CancellationSource source;
    int calls = 0;
    auto active = source.token().on_cancel([&] { ++calls; });
    auto removed = source.token().on_cancel([&] { ++calls; });
    removed.reset();

    source.cancel();
    source.cancel();

    EXPECT_TRUE(source.token().cancelled());
    EXPECT_EQ(calls, 1);
}
