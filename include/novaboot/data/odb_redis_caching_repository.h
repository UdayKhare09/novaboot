#pragma once

#include "novaboot/data/caching_crud_repository.h"
#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "novaboot/data/redis/redis_repository_base.h"
#include <chrono>
#include <string>
#include <utility>

namespace novaboot::data {

/// A helper repository class composing ODB and Redis concrete implementations
/// automatically to eliminate boilerplate in user repositories.
template<typename Entity, typename Id>
class OdbRedisCachingRepository : public CachingCrudRepository<Entity, Id> {
private:
    PgsqlRepositoryBase<Entity, Id> sql_impl_;
    RedisRepositoryBase<Entity, Id> cache_impl_;

public:
    OdbRedisCachingRepository(PgsqlDataSource& sql_ds,
                              RedisDataSource& cache_ds,
                              std::string key_prefix,
                              std::chrono::seconds default_ttl)
        : CachingCrudRepository<Entity, Id>(sql_impl_, cache_impl_, default_ttl),
          sql_impl_(sql_ds),
          cache_impl_(cache_ds, std::move(key_prefix), default_ttl) {}
};

} // namespace novaboot::data
