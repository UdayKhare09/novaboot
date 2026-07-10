#pragma once

#include "novaboot/config/app_config.h"
#include <sw/redis++/redis++.h>
#include <memory>

namespace novaboot::data {

class RedisDataSource {
public:
    explicit RedisDataSource(const config::RedisConfig& cfg);
    ~RedisDataSource() = default;

    sw::redis::Redis& client();
    sw::redis::RedisCluster& cluster_client();
    bool is_cluster() const noexcept;

private:
    config::RedisMode mode_;
    std::unique_ptr<sw::redis::Redis> redis_;
    std::unique_ptr<sw::redis::RedisCluster> cluster_;
};

} // namespace novaboot::data
