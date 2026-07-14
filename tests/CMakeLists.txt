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
novaboot_test(test_middleware          unit/test_middleware.cpp)
novaboot_test(test_jwt_middleware      unit/test_jwt_middleware.cpp)
novaboot_test(test_security_middleware unit/test_security_middleware.cpp)
novaboot_test(test_authorization_middleware unit/test_authorization_middleware.cpp)

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
novaboot_test(test_di_value           unit/test_di_value.cpp)
novaboot_test(test_di_config_bean     unit/test_di_config_bean.cpp)
novaboot_test(test_auto_routing       unit/test_auto_routing.cpp)

# ─── Integration tests ────────────────────────────────────────────────────────
novaboot_test(test_server_basic integration/test_server_basic.cpp)
novaboot_test(test_exception_handler unit/test_exception_handler.cpp)
novaboot_test(test_validation unit/test_validation.cpp)
novaboot_test(test_query_validation unit/test_query_validation.cpp)

# ─── Config unit tests ────────────────────────────────────────────────────────
novaboot_test(test_app_config unit/test_app_config.cpp)
novaboot_test(test_client_protocols unit/test_client_protocols.cpp)
novaboot_test(test_annotations unit/test_annotations.cpp)
novaboot_test(test_controller_advice unit/test_controller_advice.cpp)


