#pragma once

#include "novaboot/data/caching_crud_repository.h"
#include "model/user.h"
#include "user-odb.hxx"
#include <chrono>

#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "novaboot/data/redis/redis_repository_base.h"

/// Real database and Redis cache backed repository handling User entities.
struct [[=novaboot::di::repository{}]] UserRepository 
    : public novaboot::data::CachingCrudRepository<examples::model::User, int> {
private:
    novaboot::data::PgsqlRepositoryBase<examples::model::User, int> sql_impl_;
    novaboot::data::RedisRepositoryBase<examples::model::User, int> cache_impl_;

public:
    explicit UserRepository(novaboot::data::PgsqlDataSource& ds,
                            novaboot::data::RedisDataSource& rds)
        : novaboot::data::CachingCrudRepository<examples::model::User, int>(sql_impl_, cache_impl_, std::chrono::seconds(60)),
          sql_impl_(ds),
          cache_impl_(rds, "User", std::chrono::seconds(60)) {}
};
