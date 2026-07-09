# ─── NovaBoot Benchmarks CMake ──────────────────────────────────────────────

# Find Google Benchmark
find_package(benchmark REQUIRED)

# Benchmark Targets
add_executable(bench_router bench_router.cpp)
target_link_libraries(bench_router PRIVATE novaboot::novaboot benchmark::benchmark)

# Add standard compile options for optimization
target_compile_options(bench_router PRIVATE -O3)
