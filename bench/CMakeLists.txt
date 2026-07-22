# ─── NovaBoot Benchmarks CMake ──────────────────────────────────────────────

# Find Google Benchmark
find_package(benchmark REQUIRED)

# Benchmark Targets
add_executable(bench_router bench_router.cpp)
target_link_libraries(bench_router PRIVATE novaboot::novaboot benchmark::benchmark)

add_executable(bench_database_batch bench_database_batch.cpp)
target_link_libraries(bench_database_batch PRIVATE novaboot::sqlite benchmark::benchmark)

# Add standard compile options for optimization
target_compile_options(bench_router PRIVATE -O3)
target_compile_options(bench_database_batch PRIVATE -O3)
