#pragma once

#include <optional>
#include <chrono>

namespace novaboot::data {

template<typename Entity, typename Id>
struct CacheRepository {
    virtual std::optional<Entity> get(const Id& id)        = 0;
    virtual void                  put(const Id& id,
                                      const Entity& e,
                                      std::chrono::seconds ttl) = 0;
    virtual void                  evict(const Id& id)      = 0;
    virtual bool                  exists(const Id& id)     = 0;
    virtual ~CacheRepository() = default;
};

} // namespace novaboot::data
