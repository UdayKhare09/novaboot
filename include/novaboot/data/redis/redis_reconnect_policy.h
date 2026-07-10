#pragma once

#include "novaboot/data/exceptions.h"
#include <sw/redis++/errors.h>
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

namespace novaboot::data::redis {

template<typename F>
auto with_retry(F&& fn, int max_retries = 3, std::chrono::milliseconds initial_backoff = std::chrono::milliseconds(100)) 
    -> decltype(fn()) 
{
    std::chrono::milliseconds backoff = initial_backoff;
    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        try {
            return fn();
        } catch (const sw::redis::Error& e) {
            spdlog::warn("Redis error on attempt {}/{}: {} (backing off {}ms)", 
                         attempt, max_retries, e.what(), backoff.count());
            if (attempt == max_retries) {
                throw CacheUnavailableException(std::string("Redis operation failed after maximum retries: ") + e.what());
            }
            std::this_thread::sleep_for(backoff);
            backoff *= 2; // exponential backoff
        }
    }
    throw CacheUnavailableException("Redis operation failed with retry exhaustion");
}

} // namespace novaboot::data::redis
