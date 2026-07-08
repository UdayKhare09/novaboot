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

# spdlog (logging)
find_package(spdlog REQUIRED)

# Google Test (only when tests are enabled)
if(NOVABOOT_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

# Google Benchmark (only when benchmarks are enabled)
if(NOVABOOT_BUILD_BENCH)
    find_package(benchmark REQUIRED)
endif()
