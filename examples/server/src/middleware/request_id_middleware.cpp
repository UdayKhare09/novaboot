#include "middleware/request_id_middleware.h"

#include <array>
#include <cstdint>
#include <format>
#include <random>

// ─── UUID v4 generator ────────────────────────────────────────────────────────

std::string RequestIdMiddleware::generate_id() {
    // thread_local RNG so no locking needed on the shard thread
    thread_local std::mt19937_64 rng{std::random_device{}()};
    thread_local std::uniform_int_distribution<uint64_t> dist;

    const uint64_t hi = dist(rng);
    const uint64_t lo = dist(rng);

    // Patch in version (4) and variant bits (10xx)
    const uint64_t hi_v4  = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    const uint64_t lo_var = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    return std::format(
        "{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        static_cast<uint32_t>(hi_v4 >> 32),
        static_cast<uint16_t>(hi_v4 >> 16),
        static_cast<uint16_t>(hi_v4),
        static_cast<uint16_t>(lo_var >> 48),
        lo_var & 0x0000FFFFFFFFFFFFull);
}

// ─── handle ──────────────────────────────────────────────────────────────────

void RequestIdMiddleware::handle(novaboot::http3::Request&          req,
                                  novaboot::http3::Response&         res,
                                  novaboot::context::RequestContext& ctx,
                                  Next                               next) {
    // Re-use caller's ID or mint a fresh one
    auto existing = req.header("x-request-id");
    std::string id = existing ? std::string(*existing) : generate_id();

    // Make available to downstream handlers via the context store
    ctx.set_string("request_id", id);

    // Echo the ID back in the response
    res.header("X-Request-Id", id);

    next();
}
