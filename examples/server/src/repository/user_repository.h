#pragma once

#include "novaboot/data/caching_crud_repository.h"
#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "novaboot/data/redis/redis_repository_base.h"
#include "model/user.h"
#include "user-odb.hxx"
#include <chrono>

using namespace novaboot;
using namespace novaboot::data;
using examples::model::User;

/// Concrete Postgres DB repository for User, registered as a component.
struct UserSqlRepository
    : public PgsqlRepositoryBase<User, int> {
public:
    explicit UserSqlRepository(PgsqlDataSource& ds)
        : PgsqlRepositoryBase<User, int>(ds) {}
};

/// Concrete Redis Cache repository for User, registered as a component.
struct UserCacheRepository
    : public RedisRepositoryBase<User, int> {
public:
    explicit UserCacheRepository(RedisDataSource& ds)
        : RedisRepositoryBase<User, int>(ds, "User", std::chrono::seconds(60)) {}
};

/// Real database and Redis cache backed repository handling User entities via abstract interfaces.
struct UserRepository
    : public CachingCrudRepository<User, int> {
public:
    explicit UserRepository(CrudRepository<User, int>& sql_repo,
                            CacheRepository<User, int>& cache_repo)
        : CachingCrudRepository<User, int>(sql_repo, cache_repo, std::chrono::seconds(60)) {}
};

