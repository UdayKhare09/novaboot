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
