#include "novaboot/data/redis/redis_data_source.h"
#include <stdexcept>
#include <sstream>

namespace novaboot::data {

namespace {
struct HostPort {
    std::string host = "127.0.0.1";
    int port = 6379;
};

HostPort parse_node(const std::string& node_str) {
    auto colon = node_str.find(':');
    if (colon != std::string::npos) {
        try {
            return {
                node_str.substr(0, colon),
                std::stoi(node_str.substr(colon + 1))
            };
        } catch (...) {
            // fallback
        }
    }
    return {node_str, 6379};
}

sw::redis::Role to_redis_role(config::ReadFrom rf) {
    switch (rf) {
        case config::ReadFrom::Master:
            return sw::redis::Role::MASTER;
        case config::ReadFrom::ReplicaPreferred:
            return sw::redis::Role::SLAVE;
        case config::ReadFrom::MasterPreferred:
            return sw::redis::Role::MASTER;
    }
    return sw::redis::Role::MASTER;
}
} // namespace

RedisDataSource::RedisDataSource(const config::RedisConfig& cfg) : mode_(cfg.mode) {
    sw::redis::ConnectionOptions opts;
    HostPort first_node;
    if (!cfg.nodes.empty()) {
        first_node = parse_node(cfg.nodes[0]);
    }
    opts.host = first_node.host;
    opts.port = first_node.port;
    opts.password = cfg.password;

    sw::redis::ConnectionPoolOptions pool_opts;
    pool_opts.size = cfg.pool_size;
    pool_opts.wait_timeout = std::chrono::milliseconds(cfg.pool_timeout_ms);

    if (mode_ == config::RedisMode::Cluster) {
        sw::redis::ClusterOptions cluster_opts;
        cluster_opts.slot_map_refresh_interval = std::chrono::milliseconds(cfg.slot_refresh_interval_ms);
        auto role = to_redis_role(cfg.read_from);
        cluster_ = std::make_unique<sw::redis::RedisCluster>(opts, pool_opts, role, cluster_opts);
    } else {
        redis_ = std::make_unique<sw::redis::Redis>(opts, pool_opts);
    }
}

sw::redis::Redis& RedisDataSource::client() {
    if (!redis_) {
        throw std::runtime_error("Redis client not initialized (running in cluster mode)");
    }
    return *redis_;
}

sw::redis::RedisCluster& RedisDataSource::cluster_client() {
    if (!cluster_) {
        throw std::runtime_error("Redis Cluster client not initialized (running in single-node/sentinel mode)");
    }
    return *cluster_;
}

bool RedisDataSource::is_cluster() const noexcept {
    return mode_ == config::RedisMode::Cluster;
}

} // namespace novaboot::data
