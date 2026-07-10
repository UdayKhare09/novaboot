#pragma once

#include "novaboot/data/cache_repository.h"
#include "novaboot/data/redis/redis_data_source.h"
#include "novaboot/novaboot.h" // for json::serialize / deserialize
#include "novaboot/data/redis/redis_reconnect_policy.h"
#include <sstream>
#include <string>
#include <chrono>

namespace novaboot::data {

template<typename Entity, typename Id>
class RedisRepositoryBase : public CacheRepository<Entity, Id> {
public:
    explicit RedisRepositoryBase(RedisDataSource& ds,
                                 std::string key_prefix,
                                 std::chrono::seconds default_ttl)
        : ds_(ds), key_prefix_(std::move(key_prefix)), default_ttl_(default_ttl) {}

    std::optional<Entity> get(const Id& id) override {
        auto key = make_key(id);
        return redis::with_retry([&]() -> std::optional<Entity> {
            if (ds_.is_cluster()) {
                auto val = ds_.cluster_client().get(key);
                if (val) return novaboot::json::deserialize<Entity>(*val);
            } else {
                auto val = ds_.client().get(key);
                if (val) return novaboot::json::deserialize<Entity>(*val);
            }
            return std::nullopt;
        });
    }

    void put(const Id& id, const Entity& e, std::chrono::seconds ttl) override {
        auto key = make_key(id);
        auto val = novaboot::json::serialize(e);
        redis::with_retry([&]() {
            if (ds_.is_cluster()) {
                ds_.cluster_client().set(key, val, std::chrono::milliseconds(ttl));
            } else {
                ds_.client().set(key, val, std::chrono::milliseconds(ttl));
            }
        });
    }

    void evict(const Id& id) override {
        auto key = make_key(id);
        redis::with_retry([&]() {
            if (ds_.is_cluster()) {
                ds_.cluster_client().del(key);
            } else {
                ds_.client().del(key);
            }
        });
    }

    bool exists(const Id& id) override {
        auto key = make_key(id);
        return redis::with_retry([&]() {
            if (ds_.is_cluster()) {
                return ds_.cluster_client().exists(key) > 0;
            } else {
                return ds_.client().exists(key) > 0;
            }
        });
    }

private:
    std::string make_key(const Id& id) const {
        std::ostringstream oss;
        oss << key_prefix_ << ":" << id;
        return oss.str();
    }

    RedisDataSource& ds_;
    std::string key_prefix_;
    std::chrono::seconds default_ttl_;
};

} // namespace novaboot::data
