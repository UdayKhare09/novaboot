#pragma once

#include "novaboot/config/app_config.h"
#include "novaboot/data/pgsql/pgsql_data_source.h"
#include "novaboot/data/redis/redis_data_source.h"
#include "novaboot/di/attributes.h"

namespace novaboot::data {

struct [[=novaboot::di::module_tag{}]] DataModule {
    [[=novaboot::di::bean{}]]
    PgsqlDataSource make_pgsql(config::AppConfig& cfg) {
        return PgsqlDataSource(cfg.postgres());
    }

    [[=novaboot::di::bean{}]]
    RedisDataSource make_redis(config::AppConfig& cfg) {
        return RedisDataSource(cfg.redis());
    }

};

} // namespace novaboot::data
