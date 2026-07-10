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
novaboot_test(test_exception_handler unit/test_exception_handler.cpp)
novaboot_test(test_validation unit/test_validation.cpp)
novaboot_test(test_lombok unit/test_lombok.cpp)
target_include_directories(test_lombok PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/src)
add_dependencies(test_lombok sample_app)

# ─── Config unit tests ────────────────────────────────────────────────────────
novaboot_test(test_app_config unit/test_app_config.cpp)

# ─── Data Source unit tests ───────────────────────────────────────────────────
novaboot_test(test_pgsql_data_source unit/test_pgsql_data_source.cpp)
novaboot_test(test_redis_data_source unit/test_redis_data_source.cpp)

# ─── Data Repository unit tests ───────────────────────────────────────────────
novaboot_test(test_pgsql_repository unit/test_pgsql_repository.cpp)
odb_generate(test_pgsql_repository ${CMAKE_CURRENT_SOURCE_DIR}/unit/db_user.h)

novaboot_test(test_redis_repository unit/test_redis_repository.cpp)
novaboot_test(test_caching_crud unit/test_caching_crud.cpp)


