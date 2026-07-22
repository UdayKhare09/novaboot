# ─── NovaBoot Tests CMake ───────────────────────────────────────────────────

# Find GTest
find_package(GTest REQUIRED)

# Helper macro to add a test executable
macro(novaboot_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE novaboot::novaboot GTest::gtest GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# Labels are deliberately assigned after the tests are declared so a test may
# participate in more than one focused slice (for example, WebSocket is both
# web transport and real-time behaviour).
function(novaboot_test_slice label)
    set_tests_properties(${ARGN} PROPERTIES LABELS "${label};unit")
endfunction()

# ─── Phase 1 unit tests ──────────────────────────────────────────────────────
novaboot_test(test_arena_allocator   unit/test_arena_allocator.cpp)
novaboot_test(test_shutdown_coordinator unit/test_shutdown_coordinator.cpp)
novaboot_test(test_http_drain_gate unit/test_http_drain_gate.cpp)
novaboot_test(test_error_response unit/test_error_response.cpp)
novaboot_test(test_router            unit/test_router.cpp)
novaboot_test(test_websocket         unit/test_websocket.cpp)
novaboot_test(test_realtime_test_clients unit/test_realtime_test_clients.cpp)
novaboot_test(test_stomp             unit/test_stomp.cpp)
novaboot_test(test_header_map        unit/test_header_map.cpp)
novaboot_test(test_static_resource   unit/test_static_resource.cpp)
novaboot_test(test_multipart         unit/test_multipart.cpp)
novaboot_test(test_content_negotiation unit/test_content_negotiation.cpp)
novaboot_test(test_sse               unit/test_sse.cpp)
novaboot_test(test_middleware_pipeline unit/test_middleware_pipeline.cpp)
novaboot_test(test_middleware          unit/test_middleware.cpp)
novaboot_test(test_jwt_middleware      unit/test_jwt_middleware.cpp)
novaboot_test(test_security_middleware unit/test_security_middleware.cpp)
novaboot_test(test_authorization_middleware unit/test_authorization_middleware.cpp)
novaboot_test(test_request_id_middleware   unit/test_request_id_middleware.cpp)
novaboot_test(test_cookie                  unit/test_cookie.cpp)
novaboot_test(test_csrf_middleware         unit/test_csrf_middleware.cpp)
novaboot_test(test_session_middleware      unit/test_session_middleware.cpp)
novaboot_test(test_rate_limit_middleware   unit/test_rate_limit_middleware.cpp)
novaboot_test(test_concurrency_limit_middleware unit/test_concurrency_limit_middleware.cpp)
novaboot_test(test_trusted_forwarded_headers_middleware unit/test_trusted_forwarded_headers_middleware.cpp)
novaboot_test(test_observability        unit/test_observability.cpp)
novaboot_test(test_actuator             unit/test_actuator.cpp)

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
novaboot_test(test_auto_middleware    unit/test_auto_middleware.cpp)
novaboot_test(test_orm_reflection     unit/test_orm_reflection.cpp)
novaboot_test(test_database_crud     unit/test_database_crud.cpp)
target_link_libraries(test_database_crud PRIVATE novaboot::sqlite)

novaboot_test(test_database_observability unit/test_database_observability.cpp)
target_link_libraries(test_database_observability PRIVATE novaboot::sqlite)

novaboot_test(test_connection_pool unit/test_connection_pool.cpp)
target_link_libraries(test_connection_pool PRIVATE novaboot::sqlite)

novaboot_test(test_database_batch unit/test_database_batch.cpp)
target_link_libraries(test_database_batch PRIVATE novaboot::sqlite)

novaboot_test(test_postgres_crud     unit/test_postgres_crud.cpp)
target_link_libraries(test_postgres_crud PRIVATE novaboot::postgres)
target_include_directories(test_postgres_crud PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

novaboot_test(test_portable_types     unit/test_portable_types.cpp)
target_link_libraries(test_portable_types PRIVATE novaboot::sqlite novaboot::postgres)
target_include_directories(test_portable_types PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

novaboot_test(test_jpa_annotations    unit/test_jpa_annotations.cpp)
target_link_libraries(test_jpa_annotations PRIVATE novaboot::sqlite)

novaboot_test(test_schema_generation  unit/test_schema_generation.cpp)
target_link_libraries(test_schema_generation PRIVATE novaboot::sqlite)

# ─── Integration tests ────────────────────────────────────────────────────────
novaboot_test(test_server_basic integration/test_server_basic.cpp)
novaboot_test(test_exception_handler unit/test_exception_handler.cpp)
novaboot_test(test_validation unit/test_validation.cpp)
novaboot_test(test_query_validation unit/test_query_validation.cpp)
novaboot_test(test_app_test_client unit/test_app_test_client.cpp)
novaboot_test(test_openapi unit/test_openapi.cpp)
novaboot_test(test_live_server unit/test_live_server.cpp)

# ─── Config unit tests ────────────────────────────────────────────────────────
novaboot_test(test_app_config unit/test_app_config.cpp)
novaboot_test(test_client_protocols unit/test_client_protocols.cpp)
novaboot_test(test_annotations unit/test_annotations.cpp)
novaboot_test(test_controller_advice unit/test_controller_advice.cpp)

# ─── Focused CTest slices ────────────────────────────────────────────────────
# Run one with: ctest --test-dir build -L <label> --output-on-failure
novaboot_test_slice(web
    test_error_response test_router test_header_map test_static_resource
    test_multipart test_content_negotiation test_sse test_middleware_pipeline test_middleware test_request_id_middleware
    test_request_context test_json_reflection test_auto_routing test_server_basic
    test_exception_handler test_validation test_query_validation test_app_test_client
    test_controller_advice)

novaboot_test_slice(security
    test_jwt_middleware test_security_middleware test_authorization_middleware
    test_cookie test_csrf_middleware test_session_middleware test_rate_limit_middleware
    test_concurrency_limit_middleware test_trusted_forwarded_headers_middleware)

novaboot_test_slice(data
    test_orm_reflection test_database_crud test_database_observability
    test_connection_pool test_database_batch test_postgres_crud test_portable_types test_jpa_annotations
    test_schema_generation)

novaboot_test_slice(config
    test_di_singleton test_di_prototype test_di_scopes test_di_lifecycle
    test_di_qualifiers test_di_async test_di_cycle_detection test_di_handler_injection
    test_di_value test_di_config_bean test_auto_middleware test_app_config
    test_annotations)

novaboot_test_slice(realtime
    test_websocket test_realtime_test_clients test_stomp)

novaboot_test_slice(operations
    test_shutdown_coordinator test_http_drain_gate test_observability test_actuator
    test_database_observability)

set_tests_properties(test_server_basic PROPERTIES LABELS "web;integration")
