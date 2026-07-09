#include <benchmark/benchmark.h>
#include "novaboot/router/router.h"

using namespace novaboot::router;

static Router setup_router() {
    Router r;
    auto handler = [](auto&, auto&, auto&) {};

    r.route("/").get(handler);
    r.route("/api/v1/users").get(handler);
    r.route("/api/v1/users/:id").get(handler);
    r.route("/api/v1/users/:id/posts").get(handler);
    r.route("/static/*filepath").get(handler);

    return r;
}

static void BM_RouterStaticMatch(benchmark::State& state) {
    auto r = setup_router();
    for (auto _ : state) {
        auto res = r.match("GET", "/api/v1/users");
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_RouterStaticMatch);

static void BM_RouterParamMatch(benchmark::State& state) {
    auto r = setup_router();
    for (auto _ : state) {
        auto res = r.match("GET", "/api/v1/users/12345/posts");
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_RouterParamMatch);

static void BM_RouterWildcardMatch(benchmark::State& state) {
    auto r = setup_router();
    for (auto _ : state) {
        auto res = r.match("GET", "/static/images/logo.png");
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_RouterWildcardMatch);

static void BM_RouterMiss(benchmark::State& state) {
    auto r = setup_router();
    for (auto _ : state) {
        auto res = r.match("GET", "/api/v1/notfound");
        benchmark::DoNotOptimize(res);
    }
}
BENCHMARK(BM_RouterMiss);

BENCHMARK_MAIN();
