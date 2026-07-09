#pragma once

#include "novaboot/di/di.h"
#include <vector>
#include <string>
#include <spdlog/spdlog.h>

/// Request-scoped bean: a new instance is created for each HTTP request
struct [[=novaboot::di::component{}]]
       [[=novaboot::di::scoped{novaboot::di::Scope::Request}]]
RequestLogger {
    std::vector<std::string> logs;

    RequestLogger() {
        spdlog::debug("RequestLogger instance created for new request.");
    }

    void log(std::string_view message) {
        logs.emplace_back(message);
        spdlog::info("[RequestLog] {}", message);
    }

    ~RequestLogger() {
        spdlog::debug("RequestLogger instance destroyed.");
    }
};
