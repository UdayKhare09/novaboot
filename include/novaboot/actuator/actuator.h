#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "novaboot/http3/request.h"
#include "novaboot/observability/observation.h"

namespace novaboot::middleware { class Middleware; }
namespace novaboot::router { class Router; }

namespace novaboot::actuator {

enum class HealthStatus { Up, Down, OutOfService, Unknown };

struct Health {
    HealthStatus status = HealthStatus::Unknown;
    std::map<std::string, std::string> details;
};

enum class Availability { Starting, Ready, RefusingTraffic, Stopped };

/// Configures a small, separately exposed management surface.
struct Config {
    std::string base_path = "/actuator";
    bool expose_health = true;
    bool expose_info = true;
    bool expose_metrics = false;
    bool expose_prometheus = false;
    bool expose_config = false;
    bool expose_loggers = false;
    bool expose_observations = false;
    std::string disk_path = "/";
    std::uint64_t disk_min_free_bytes = 0;
    bool require_authorization = false;
    std::function<bool(const http3::Request&)> authorizer;
};

/// Actuator-style operations endpoints with a vendor-neutral signal source.
class Actuator {
public:
    explicit Actuator(Config config = {});

    [[nodiscard]] std::shared_ptr<observability::MeterRegistry> meters() const;
    [[nodiscard]] std::shared_ptr<middleware::Middleware> observation_middleware() const;

    void add_health_contributor(std::string name, std::function<Health()> contributor);
    void add_info(std::string key, std::string value);
    /// Adds an effective configuration source for the guarded config endpoint.
    /// Secret-like property names are masked before serialization.
    void add_config_source(std::string name, std::map<std::string, std::string> properties);
    void register_routes(router::Router& router);

    void set_availability(Availability availability) noexcept;
    [[nodiscard]] Availability availability() const noexcept;

private:
    [[nodiscard]] bool authorized(const http3::Request& request) const;
    [[nodiscard]] Health liveness() const;
    [[nodiscard]] Health readiness() const;
    [[nodiscard]] Health overall_health() const;

    Config config_;
    std::shared_ptr<observability::MeterRegistry> meters_;
    std::shared_ptr<middleware::Middleware> observation_middleware_;
    std::map<std::string, std::function<Health()>> health_contributors_;
    std::map<std::string, std::string> info_;
    std::map<std::string, std::map<std::string, std::string>> config_sources_;
    std::atomic<Availability> availability_{Availability::Starting};
    bool routes_registered_ = false;
};

} // namespace novaboot::actuator
