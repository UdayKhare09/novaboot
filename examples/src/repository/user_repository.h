#pragma once

#include "novaboot/data/caching_crud_repository.h"
#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "novaboot/data/redis/redis_repository_base.h"
#include "model/user.h"
#include "user-odb.hxx"
#include <chrono>

/// Concrete Postgres DB repository for User, registered as a component.
struct [[=novaboot::di::repository{}]] UserSqlRepository 
    : public novaboot::data::PgsqlRepositoryBase<examples::model::User, int> {
public:
    explicit UserSqlRepository(novaboot::data::PgsqlDataSource& ds)
        : novaboot::data::PgsqlRepositoryBase<examples::model::User, int>(ds) {}
};

/// Concrete Redis Cache repository for User, registered as a component.
struct [[=novaboot::di::repository{}]] UserCacheRepository 
    : public novaboot::data::RedisRepositoryBase<examples::model::User, int> {
public:
    explicit UserCacheRepository(novaboot::data::RedisDataSource& ds)
        : novaboot::data::RedisRepositoryBase<examples::model::User, int>(ds, "User", std::chrono::seconds(60)) {}
};

/// Real database and Redis cache backed repository handling User entities via abstract interfaces.
struct [[=novaboot::di::repository{}]] UserRepository 
    : public novaboot::data::CachingCrudRepository<examples::model::User, int> {
public:
    explicit UserRepository(novaboot::data::CrudRepository<examples::model::User, int>& sql_repo,
                            novaboot::data::CacheRepository<examples::model::User, int>& cache_repo)
        : novaboot::data::CachingCrudRepository<examples::model::User, int>(sql_repo, cache_repo, std::chrono::seconds(60)) {}
};
