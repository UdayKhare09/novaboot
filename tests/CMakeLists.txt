# ─── NovaBoot Tests CMake ───────────────────────────────────────────────────

# Find GTest
find_package(GTest REQUIRED)

# Helper macro to add a test executable
macro(novaboot_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE novaboot::novaboot GTest::gtest GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# ─── Phase 1 unit tests ──────────────────────────────────────────────────────
novaboot_test(test_arena_allocator   unit/test_arena_allocator.cpp)
novaboot_test(test_router            unit/test_router.cpp)
novaboot_test(test_header_map        unit/test_header_map.cpp)
novaboot_test(test_middleware_pipeline unit/test_middleware_pipeline.cpp)
novaboot_test(test_request_context   unit/test_request_context.cpp)
novaboot_test(test_json_reflection   unit/test_json_reflection.cpp)

# ─── Phase 2: DI/IoC unit tests ──────────────────────────────────────────────
novaboot_test(test_di_singleton       unit/test_di_singleton.cpp)
novaboot_test(test_di_prototype       unit/test_di_prototype.cpp)
novaboot_test(test_di_scopes          unit/test_di_scopes.cpp)
novaboot_test(test_di_lifecycle       unit/test_di_lifecycle.cpp)
novaboot_test(test_di_qualifiers      unit/test_di_qualifiers.cpp)
novaboot_test(test_di_async           unit/test_di_async.cpp)
novaboot_test(test_di_cycle_detection unit/test_di_cycle_detection.cpp)
novaboot_test(test_di_handler_injection unit/test_di_handler_injection.cpp)

# ─── Integration tests ────────────────────────────────────────────────────────
novaboot_test(test_server_basic integration/test_server_basic.cpp)
