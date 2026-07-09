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

add_executable(test_header_map unit/test_header_map.cpp)
target_link_libraries(test_header_map PRIVATE novaboot::novaboot GTest::gtest GTest::gtest_main)
add_test(NAME test_header_map COMMAND test_header_map)

add_executable(test_middleware_pipeline unit/test_middleware_pipeline.cpp)
target_link_libraries(test_middleware_pipeline PRIVATE novaboot::novaboot GTest::gtest GTest::gtest_main)
add_test(NAME test_middleware_pipeline COMMAND test_middleware_pipeline)

add_executable(test_server_basic integration/test_server_basic.cpp)
target_link_libraries(test_server_basic PRIVATE novaboot::novaboot GTest::gtest GTest::gtest_main)
add_test(NAME test_server_basic COMMAND test_server_basic)
