cmake_minimum_required(VERSION 3.25)

project(novaboot
    VERSION 0.1.0
    DESCRIPTION "Production-grade HTTP/3-only C++ framework"
    LANGUAGES CXX
)

# ─── C++23 required ───────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Build type defaults ──────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# ─── Options ──────────────────────────────────────────────────────────────────
option(NOVABOOT_BUILD_TESTS "Build unit tests" OFF)
option(NOVABOOT_BUILD_BENCH "Build benchmarks" OFF)
option(NOVABOOT_BUILD_EXAMPLES "Build examples" ON)
option(NOVABOOT_ASAN "Enable AddressSanitizer" OFF)

# ─── Compiler flags ──────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-march=native)
endif()

if(NOVABOOT_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ─── Dependencies ─────────────────────────────────────────────────────────────
include(cmake/Dependencies.cmake)

# ─── Library ──────────────────────────────────────────────────────────────────
add_library(novaboot STATIC
    # Memory
    src/memory/arena_allocator.cpp
    src/memory/pool_allocator.cpp

    # Net
    src/net/address.cpp
    src/net/udp_socket.cpp

    # Core
    src/core/epoll_event_loop.cpp
    src/core/shard.cpp
    src/core/server.cpp

    # QUIC
    src/quic/tls_context.cpp
    src/quic/tls_session.cpp
    src/quic/quic_connection.cpp
    src/quic/connection_manager.cpp

    # HTTP/3
    src/http3/header_map.cpp
    src/http3/request.cpp
    src/http3/response.cpp
    src/http3/http3_session.cpp
    src/http3/http3_stream.cpp

    # Router
    src/router/router.cpp

    # Middleware
    src/middleware/pipeline.cpp

    # Context
    src/context/request_context.cpp
)

add_library(novaboot::novaboot ALIAS novaboot)

target_include_directories(novaboot
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(novaboot
    PUBLIC
        Threads::Threads
    PRIVATE
        PkgConfig::NGTCP2
        PkgConfig::NGTCP2_CRYPTO_OSSL
        PkgConfig::NGHTTP3
        OpenSSL::SSL
        OpenSSL::Crypto
        spdlog::spdlog
)

target_compile_features(novaboot PUBLIC cxx_std_23)

# ─── Examples ─────────────────────────────────────────────────────────────────
if(NOVABOOT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ─── Tests ────────────────────────────────────────────────────────────────────
if(NOVABOOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Benchmarks ───────────────────────────────────────────────────────────────
if(NOVABOOT_BUILD_BENCH)
    add_subdirectory(bench)
endif()
