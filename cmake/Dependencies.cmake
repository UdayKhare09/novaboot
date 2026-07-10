# ─── Dependencies for NovaBoot ─────────────────────────────────────────────────
# All dependencies are expected to be installed on the system (Arch Linux).
# We use find_package / pkg-config to locate them.

find_package(Threads REQUIRED)
find_package(OpenSSL 3.5.0 REQUIRED)
find_package(PkgConfig REQUIRED)

# ngtcp2 (QUIC transport)
pkg_check_modules(NGTCP2 REQUIRED IMPORTED_TARGET libngtcp2)

# ngtcp2 crypto backend (OpenSSL native QUIC API — not quictls)
pkg_check_modules(NGTCP2_CRYPTO_OSSL REQUIRED IMPORTED_TARGET libngtcp2_crypto_ossl)

# nghttp3 (HTTP/3 framing)
pkg_check_modules(NGHTTP3 REQUIRED IMPORTED_TARGET libnghttp3)

# liburing (io_uring event loop backend)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)

# spdlog (logging)
find_package(spdlog REQUIRED)

# simdjson (JSON parser)
find_package(simdjson REQUIRED)

# tomlplusplus (TOML parser)
find_package(tomlplusplus REQUIRED)

# redis++ and hiredis (Redis client)
pkg_check_modules(REDIS_PP REQUIRED IMPORTED_TARGET redis++)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# libodb and libodb-pgsql (ORM)
pkg_check_modules(LIBODB REQUIRED IMPORTED_TARGET libodb)
pkg_check_modules(LIBODB_PGSQL REQUIRED IMPORTED_TARGET libodb-pgsql)

# Google Test (only when tests are enabled)
if(NOVABOOT_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

# Google Benchmark (only when benchmarks are enabled)
if(NOVABOOT_BUILD_BENCH)
    find_package(benchmark REQUIRED)
endif()
