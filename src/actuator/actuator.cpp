#include "novaboot/actuator/actuator.h"

#include <cctype>
#include <chrono>
#include <format>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

#include <sys/statvfs.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "novaboot/middleware/middleware.h"
#include "novaboot/observability/http_observation_middleware.h"
#include "novaboot/router/router.h"

namespace novaboot::actuator {
namespace {

std::string status_name(HealthStatus status) {
    switch (status) {
        case HealthStatus::Up: return "UP";
        case HealthStatus::Down: return "DOWN";
        case HealthStatus::OutOfService: return "OUT_OF_SERVICE";
        case HealthStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

int severity(HealthStatus status) {
    switch (status) {
        case HealthStatus::Down: return 4;
        case HealthStatus::OutOfService: return 3;
        case HealthStatus::Unknown: return 2;
        case HealthStatus::Up: return 1;
    }
    return 0;
}

std::string escape_json(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += character; break;
        }
    }
    return result;
}

std::string health_json(const Health& health) {
    std::string result = std::format("{{\"status\":\"{}\"", status_name(health.status));
    if (!health.details.empty()) {
        result += ",\"details\":{";
        bool first = true;
        for (const auto& [key, value] : health.details) {
            if (!first) result += ',';
            first = false;
            result += std::format("\"{}\":\"{}\"", escape_json(key), escape_json(value));
        }
        result += '}';
    }
    return result + '}';
}

std::string metrics_json(const std::vector<observability::MetricPoint>& metrics) {
    std::string result = "{\"metrics\":[";
    bool first_metric = true;
    for (const auto& metric : metrics) {
        if (!first_metric) result += ',';
        first_metric = false;
        const char* type = metric.kind == observability::InstrumentKind::Counter ? "counter" :
                           metric.kind == observability::InstrumentKind::UpDownCounter ? "updowncounter" :
                           metric.kind == observability::InstrumentKind::Histogram ? "histogram" : "gauge";
        result += std::format("{{\"name\":\"{}\",\"unit\":\"{}\",\"type\":\"{}\",\"value\":{}",
                              escape_json(metric.name), escape_json(metric.unit), type, metric.value);
        if (metric.kind == observability::InstrumentKind::Histogram) {
            result += std::format(",\"count\":{},\"sum\":{}", metric.count, metric.sum);
        }
        result += ",\"attributes\":{";
        bool first_attribute = true;
        for (const auto& [key, value] : metric.attributes) {
            if (!first_attribute) result += ',';
            first_attribute = false;
            result += std::format("\"{}\":\"{}\"", escape_json(key), escape_json(value));
        }
        result += "}}";
    }
    return result + "]}";
}

std::string prometheus_name(const observability::MetricPoint& metric) {
    std::string result;
    result.reserve(metric.name.size() + 16);
    for (const auto character : metric.name) {
        result += (std::isalnum(static_cast<unsigned char>(character)) || character == '_')
            ? character : '_';
    }
    if (metric.unit == "s") result += "_seconds";
    if (metric.unit == "By") result += "_bytes";
    if (metric.kind == observability::InstrumentKind::Counter) result += "_total";
    return result;
}

std::string escape_prometheus_label(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const auto character : value) {
        if (character == '\\') result += "\\\\";
        else if (character == '"') result += "\\\"";
        else if (character == '\n') result += "\\n";
        else result += character;
    }
    return result;
}

std::string prometheus_labels(const observability::Attributes& attributes,
                              std::string_view extra_name = {},
                              std::string_view extra_value = {}) {
    if (attributes.empty() && extra_name.empty()) return {};
    std::string result = "{";
    bool first = true;
    for (const auto& [key, value] : attributes) {
        if (!first) result += ',';
        first = false;
        std::string label_name;
        for (const auto character : key) {
            label_name += (std::isalnum(static_cast<unsigned char>(character)) || character == '_')
                ? character : '_';
        }
        result += std::format("{}=\"{}\"", label_name, escape_prometheus_label(value));
    }
    if (!extra_name.empty()) {
        if (!first) result += ',';
        result += std::format("{}=\"{}\"", extra_name, escape_prometheus_label(extra_value));
    }
    return result + '}';
}

std::string prometheus_metrics(const std::vector<observability::MetricPoint>& metrics) {
    std::string result;
    std::set<std::string> emitted_types;
    for (const auto& metric : metrics) {
        const auto name = prometheus_name(metric);
        const char* type = metric.kind == observability::InstrumentKind::Histogram ? "histogram" :
                           metric.kind == observability::InstrumentKind::Counter ? "counter" : "gauge";
        if (emitted_types.insert(name).second) {
            result += std::format("# TYPE {} {}\n", name, type);
        }
        if (metric.kind == observability::InstrumentKind::Histogram) {
            for (const auto& bucket : metric.buckets) {
                result += std::format("{}_bucket{} {}\n", name,
                    prometheus_labels(metric.attributes, "le", std::to_string(bucket.upper_bound)),
                    bucket.count);
            }
            result += std::format("{}_bucket{} {}\n", name,
                prometheus_labels(metric.attributes, "le", "+Inf"), metric.count);
            result += std::format("{}_sum{} {}\n", name, prometheus_labels(metric.attributes), metric.sum);
            result += std::format("{}_count{} {}\n", name, prometheus_labels(metric.attributes), metric.count);
        } else {
            result += std::format("{}{} {}\n", name, prometheus_labels(metric.attributes), metric.value);
        }
    }
    return result;
}

bool secret_like_key(std::string_view key) {
    std::string lower;
    lower.reserve(key.size());
    for (const auto character : key) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return lower.contains("password") || lower.contains("secret") ||
           lower.contains("token") || lower.contains("credential") ||
           lower.ends_with(".key") || lower.ends_with("_key");
}

std::string config_json(const std::map<std::string, std::map<std::string, std::string>>& sources) {
    std::string result = "{\"propertySources\":[";
    bool first_source = true;
    for (const auto& [source_name, properties] : sources) {
        if (!first_source) result += ',';
        first_source = false;
        result += std::format("{{\"name\":\"{}\",\"properties\":{{", escape_json(source_name));
        bool first_property = true;
        for (const auto& [key, value] : properties) {
            if (!first_property) result += ',';
            first_property = false;
            const auto safe_value = secret_like_key(key) ? "******" : value;
            result += std::format("\"{}\":{{\"value\":\"{}\"}}",
                                  escape_json(key), escape_json(safe_value));
        }
        result += "}}";
    }
    return result + "]}";
}

std::string observations_json(const std::shared_ptr<observability::MeterRegistry>& meters) {
    std::string result = "{\"spans\":[";
    bool first = true;
    for (const auto& span : meters->spans()) {
        if (!first) result += ',';
        first = false;
        result += std::format("{{\"name\":\"{}\",\"error\":{},\"durationSeconds\":{}}}",
            escape_json(span.name), span.error ? "true" : "false",
            std::chrono::duration<double>(span.duration).count());
    }
    return result + ",\"eventCount\":" + std::to_string(meters->events().size()) + "}";
}

std::string level_name(spdlog::level::level_enum level) {
    const auto view = spdlog::level::to_string_view(level);
    return std::string(view.data(), view.size());
}

std::optional<spdlog::level::level_enum> parse_level(std::string_view body) {
    constexpr std::string_view property = "\"configuredLevel\"";
    const auto property_position = body.find(property);
    if (property_position == std::string_view::npos) return std::nullopt;
    const auto colon = body.find(':', property_position + property.size());
    if (colon == std::string_view::npos) return std::nullopt;
    const auto quote_begin = body.find('"', colon + 1);
    if (quote_begin == std::string_view::npos) return std::nullopt;
    const auto quote_end = body.find('"', quote_begin + 1);
    if (quote_end == std::string_view::npos) return std::nullopt;
    const auto value = body.substr(quote_begin + 1, quote_end - quote_begin - 1);
    if (value == "trace") return spdlog::level::trace;
    if (value == "debug") return spdlog::level::debug;
    if (value == "info") return spdlog::level::info;
    if (value == "warn") return spdlog::level::warn;
    if (value == "error") return spdlog::level::err;
    if (value == "critical") return spdlog::level::critical;
    if (value == "off") return spdlog::level::off;
    return std::nullopt;
}

} // namespace

Actuator::Actuator(Config config)
    : config_(std::move(config)),
      meters_(std::make_shared<observability::MeterRegistry>()),
      observation_middleware_(std::make_shared<observability::HttpObservationMiddleware>(meters_)) {
    if (config_.base_path.empty() || config_.base_path.front() != '/') {
        throw std::invalid_argument("Actuator base_path must start with '/'");
    }
    while (config_.base_path.size() > 1 && config_.base_path.back() == '/') {
        config_.base_path.pop_back();
    }
    const auto process_started = std::chrono::steady_clock::now();
    health_contributors_["process"] = [process_started] {
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - process_started).count();
        return Health{HealthStatus::Up, {
            {"pid", std::to_string(getpid())},
            {"uptime_seconds", std::to_string(uptime)},
            {"available_processors", std::to_string(std::thread::hardware_concurrency())},
        }};
    };
    health_contributors_["disk"] = [disk_path = config_.disk_path,
                                    minimum_free = config_.disk_min_free_bytes] {
        struct statvfs filesystem {};
        if (statvfs(disk_path.c_str(), &filesystem) != 0) {
            return Health{HealthStatus::Down, {{"path", disk_path}, {"error", "statvfs failed"}}};
        }
        const auto free_bytes = static_cast<std::uint64_t>(filesystem.f_bavail) * filesystem.f_frsize;
        return Health{free_bytes < minimum_free ? HealthStatus::Down : HealthStatus::Up, {
            {"path", disk_path}, {"free_bytes", std::to_string(free_bytes)},
        }};
    };
}

std::shared_ptr<observability::MeterRegistry> Actuator::meters() const { return meters_; }
std::shared_ptr<middleware::Middleware> Actuator::observation_middleware() const { return observation_middleware_; }

void Actuator::add_health_contributor(std::string name, std::function<Health()> contributor) {
    health_contributors_[std::move(name)] = std::move(contributor);
}

void Actuator::add_info(std::string key, std::string value) {
    info_[std::move(key)] = std::move(value);
}

void Actuator::add_config_source(std::string name,
                                 std::map<std::string, std::string> properties) {
    config_sources_[std::move(name)] = std::move(properties);
}

bool Actuator::authorized(const http3::Request& request) const {
    return !config_.require_authorization || (config_.authorizer && config_.authorizer(request));
}

Health Actuator::liveness() const {
    return {availability() == Availability::Stopped ? HealthStatus::Down : HealthStatus::Up, {}};
}

Health Actuator::readiness() const {
    return {availability() == Availability::Ready ? HealthStatus::Up : HealthStatus::OutOfService, {}};
}

Health Actuator::overall_health() const {
    Health result = readiness();
    for (const auto& [name, contributor] : health_contributors_) {
        const auto health = contributor();
        result.details[name + ".status"] = status_name(health.status);
        for (const auto& [detail_name, detail_value] : health.details) {
            result.details[name + "." + detail_name] = detail_value;
        }
        if (severity(health.status) > severity(result.status)) result.status = health.status;
    }
    return result;
}

void Actuator::register_routes(router::Router& router) {
    if (routes_registered_) throw std::logic_error("Actuator routes are already registered");
    routes_registered_ = true;
    const auto deny = [](http3::Response& response) { response.status(403).json("{\"error\":\"management access denied\"}"); };
    if (config_.expose_health) {
        router.route(config_.base_path + "/health").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            response.json(health_json(overall_health()));
        });
        router.route(config_.base_path + "/health/liveness").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            response.json(health_json(liveness()));
        });
        router.route(config_.base_path + "/health/readiness").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            const auto health = readiness();
            response.status(health.status == HealthStatus::Up ? 200 : 503).json(health_json(health));
        });
    }
    if (config_.expose_info) {
        router.route(config_.base_path + "/info").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            std::string body = "{";
            bool first = true;
            for (const auto& [key, value] : info_) {
                if (!first) body += ',';
                first = false;
                body += std::format("\"{}\":\"{}\"", escape_json(key), escape_json(value));
            }
            response.json(body + "}");
        });
    }
    if (config_.expose_metrics) {
        router.route(config_.base_path + "/metrics").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            response.json(metrics_json(meters_->snapshot()));
        });
    }
    if (config_.expose_prometheus) {
        router.route(config_.base_path + "/prometheus").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            response.header("content-type", "text/plain; version=0.0.4; charset=utf-8")
                    .body(prometheus_metrics(meters_->snapshot()));
        });
    }
    if (config_.expose_config) {
        router.route(config_.base_path + "/configprops").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            response.json(config_json(config_sources_));
        });
    }
    if (config_.expose_loggers) {
        router.route(config_.base_path + "/loggers").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            response.json(std::format("{{\"ROOT\":{{\"configuredLevel\":\"{}\"}}}}",
                                      level_name(spdlog::default_logger()->level())));
        });
        router.route(config_.base_path + "/loggers/ROOT")
            .get([this, deny](auto& request, auto& response, auto&) {
                if (!authorized(request)) return deny(response);
                response.json(std::format("{{\"configuredLevel\":\"{}\"}}",
                                          level_name(spdlog::default_logger()->level())));
            })
            .post([this, deny](auto& request, auto& response, auto&) {
                if (!authorized(request)) return deny(response);
                const auto level = parse_level(request.body());
                if (!level) {
                    response.status(400).json("{\"error\":\"configuredLevel must be a supported lowercase level\"}");
                    return;
                }
                spdlog::set_level(*level);
                response.json(std::format("{{\"configuredLevel\":\"{}\"}}", level_name(*level)));
            });
    }
    if (config_.expose_observations) {
        router.route(config_.base_path + "/observations").get([this, deny](auto& request, auto& response, auto&) {
            if (!authorized(request)) return deny(response);
            response.json(observations_json(meters_));
        });
    }
}

void Actuator::set_availability(Availability availability) noexcept { availability_.store(availability); }
Availability Actuator::availability() const noexcept { return availability_.load(); }

} // namespace novaboot::actuator
