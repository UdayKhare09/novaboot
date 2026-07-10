#include "novaboot/config/app_config.h"
#include <toml++/toml.h>
#include <stdexcept>
#include <iostream>

namespace novaboot::config {

struct AppConfig::Impl {
    toml::table table;
};

AppConfig AppConfig::load(const std::string& path) {
    AppConfig cfg;
    cfg.impl_ = std::make_shared<Impl>();
    try {
        cfg.impl_->table = toml::parse_file(path);
    } catch (const toml::parse_error& err) {
        throw std::runtime_error("Failed to parse TOML config file '" + path + "': " + std::string(err.description()));
    }

    const auto& tbl = cfg.impl_->table;

    // Parse [server] section
    if (auto server = tbl["server"].as_table()) {
        if (auto host = server->get("host")) {
            if (auto str = host->as_string()) cfg.server_.host = str->get();
        }
        if (auto port = server->get("port")) {
            if (auto val = port->as_integer()) cfg.server_.port = static_cast<uint16_t>(val->get());
        }
        if (auto workers = server->get("workers")) {
            if (auto val = workers->as_integer()) cfg.server_.workers = static_cast<uint32_t>(val->get());
        }
        if (auto tls_cert = server->at_path("tls.cert").as_string()) {
            cfg.server_.tls_cert = tls_cert->get();
        }
        if (auto tls_key = server->at_path("tls.key").as_string()) {
            cfg.server_.tls_key = tls_key->get();
        }
        if (auto static_res = server->get("static_resources")) {
            if (auto str = static_res->as_string()) cfg.server_.static_resources = str->get();
        }
    }

    // Parse [datasource.postgres] section
    if (auto pg = tbl.at_path("datasource.postgres").as_table()) {
        if (auto host = pg->get("host")) {
            if (auto str = host->as_string()) cfg.postgres_.host = str->get();
        }
        if (auto port = pg->get("port")) {
            if (auto val = port->as_integer()) cfg.postgres_.port = static_cast<uint16_t>(val->get());
        }
        if (auto user = pg->get("user")) {
            if (auto str = user->as_string()) cfg.postgres_.user = str->get();
        }
        if (auto password = pg->get("password")) {
            if (auto str = password->as_string()) cfg.postgres_.password = str->get();
        }
        if (auto database = pg->get("database")) {
            if (auto str = database->as_string()) cfg.postgres_.database = str->get();
        }
        if (auto pool_min = pg->at_path("pool.min").as_integer()) {
            cfg.postgres_.pool_min = static_cast<uint32_t>(pool_min->get());
        }
        if (auto pool_max = pg->at_path("pool.max").as_integer()) {
            cfg.postgres_.pool_max = static_cast<uint32_t>(pool_max->get());
        }
    }

    // Parse [datasource.redis] section
    if (auto redis = tbl.at_path("datasource.redis").as_table()) {
        if (auto mode = redis->get("mode")) {
            if (auto str = mode->as_string()) {
                std::string mode_str = str->get();
                if (mode_str == "cluster") {
                    cfg.redis_.mode = RedisMode::Cluster;
                } else if (mode_str == "sentinel") {
                    cfg.redis_.mode = RedisMode::Sentinel;
                } else {
                    cfg.redis_.mode = RedisMode::Single;
                }
            }
        }

        if (auto nodes = redis->get("nodes")) {
            if (auto nodes_arr = nodes->as_array()) {
                for (auto&& node : *nodes_arr) {
                    if (auto str = node.as_string()) {
                        cfg.redis_.nodes.push_back(str->get());
                    }
                }
            }
        }

        if (auto password = redis->get("password")) {
            if (auto str = password->as_string()) cfg.redis_.password = str->get();
        }
        if (auto pool_size = redis->at_path("pool.size").as_integer()) {
            cfg.redis_.pool_size = static_cast<uint32_t>(pool_size->get());
        }
        if (auto timeout = redis->at_path("pool.timeout_ms").as_integer()) {
            cfg.redis_.pool_timeout_ms = static_cast<uint32_t>(timeout->get());
        }
        if (auto read_from = redis->get("read_from")) {
            if (auto str = read_from->as_string()) {
                std::string read_from_str = str->get();
                if (read_from_str == "master") {
                    cfg.redis_.read_from = ReadFrom::Master;
                } else if (read_from_str == "master_preferred") {
                    cfg.redis_.read_from = ReadFrom::MasterPreferred;
                } else {
                    cfg.redis_.read_from = ReadFrom::ReplicaPreferred;
                }
            }
        }
        if (auto slot_ref = redis->at_path("cluster.slot_refresh_interval_ms").as_integer()) {
            cfg.redis_.slot_refresh_interval_ms = static_cast<uint32_t>(slot_ref->get());
        }
    }

    return cfg;
}

std::optional<std::string> AppConfig::get_string(std::string_view key) const {
    if (!impl_) return std::nullopt;
    if (auto node = impl_->table.at_path(key)) {
        if (auto val = node.as_string()) {
            return val->get();
        }
    }
    return std::nullopt;
}

std::optional<int64_t> AppConfig::get_int(std::string_view key) const {
    if (!impl_) return std::nullopt;
    if (auto node = impl_->table.at_path(key)) {
        if (auto val = node.as_integer()) {
            return val->get();
        }
    }
    return std::nullopt;
}

std::optional<double> AppConfig::get_double(std::string_view key) const {
    if (!impl_) return std::nullopt;
    if (auto node = impl_->table.at_path(key)) {
        if (auto val = node.as_floating_point()) {
            return val->get();
        } else if (auto val_i = node.as_integer()) {
            return static_cast<double>(val_i->get());
        }
    }
    return std::nullopt;
}

std::optional<bool> AppConfig::get_bool(std::string_view key) const {
    if (!impl_) return std::nullopt;
    if (auto node = impl_->table.at_path(key)) {
        if (auto val = node.as_boolean()) {
            return val->get();
        }
    }
    return std::nullopt;
}

} // namespace novaboot::config
