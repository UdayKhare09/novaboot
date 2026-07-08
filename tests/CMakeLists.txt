# ─── NovaBoot Tests CMake ───────────────────────────────────────────────────

# Find GTest
find_package(GTest REQUIRED)

# Test Targets
add_executable(test_arena_allocator unit/test_arena_allocator.cpp)
target_link_libraries(test_arena_allocator PRIVATE novaboot::novaboot GTest::gtest GTest::gtest_main)
add_test(NAME test_arena_allocator COMMAND test_arena_allocator)

add_executable(test_router unit/test_router.cpp)
target_link_libraries(test_router PRIVATE novaboot::novaboot GTest::gtest GTest::gtest_main)
add_test(NAME test_router COMMAND test_router)
