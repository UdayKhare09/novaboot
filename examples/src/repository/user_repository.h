#pragma once

#include "novaboot/data/odb_redis_caching_repository.h"
#include "model/user.h"
#include "user-odb.hxx"
#include <chrono>

/// Real database and Redis cache backed repository handling User entities.
struct [[=novaboot::di::repository{}]] UserRepository 
    : public novaboot::data::OdbRedisCachingRepository<examples::model::User, int> {
public:
    explicit UserRepository(novaboot::data::PgsqlDataSource& ds,
                            novaboot::data::RedisDataSource& rds)
        : novaboot::data::OdbRedisCachingRepository<examples::model::User, int>(ds, rds, "User", std::chrono::seconds(60)) {}
};
