#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <string_view>
#include <optional>

namespace novaboot::config {

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 4433;
    uint32_t workers = 0;
    std::string tls_cert = "cert.pem";
    std::string tls_key = "key.pem";
    std::string static_resources = "examples/server/src/resources/static";
};

class AppConfig {
public:
    AppConfig() = default;
    
    static AppConfig load(const std::string& path);

    const ServerConfig& server() const noexcept { return server_; }

    template<typename T>
    std::optional<T> get(std::string_view key) const;

private:
    std::optional<std::string> get_string(std::string_view key) const;
    std::optional<int64_t>     get_int(std::string_view key) const;
    std::optional<double>      get_double(std::string_view key) const;
    std::optional<bool>        get_bool(std::string_view key) const;

    ServerConfig server_;

    struct Impl;
    std::shared_ptr<Impl> impl_;
};

template<>
inline std::optional<std::string> AppConfig::get<std::string>(std::string_view key) const {
    return get_string(key);
}

template<>
inline std::optional<int> AppConfig::get<int>(std::string_view key) const {
    auto val = get_int(key);
    return val ? std::optional<int>(static_cast<int>(*val)) : std::nullopt;
}

template<>
inline std::optional<int64_t> AppConfig::get<int64_t>(std::string_view key) const {
    return get_int(key);
}

template<>
inline std::optional<double> AppConfig::get<double>(std::string_view key) const {
    return get_double(key);
}

template<>
inline std::optional<bool> AppConfig::get<bool>(std::string_view key) const {
    return get_bool(key);
}

} // namespace novaboot::config
