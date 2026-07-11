#include "novaboot/http3/request.h"

#include <algorithm>

namespace novaboot::http {

std::optional<std::string_view>
Request::query_param(std::string_view name) const {
    auto qs = query_string();
    if (qs.empty()) return std::nullopt;

    // Simple query string parser
    std::string_view remaining = qs;
    while (!remaining.empty()) {
        auto amp = remaining.find('&');
        auto pair = remaining.substr(0, amp);

        auto eq = pair.find('=');
        auto key = pair.substr(0, eq);

        if (key == name) {
            if (eq != std::string_view::npos) {
                return pair.substr(eq + 1);
            }
            return std::string_view{}; // Key exists but no value
        }

        if (amp == std::string_view::npos) break;
        remaining = remaining.substr(amp + 1);
    }

    return std::nullopt;
}

std::string_view Request::query_string() const noexcept {
    auto qpos = path_.find('?');
    if (qpos == std::string::npos) {
        return {};
    }
    return std::string_view(path_).substr(qpos + 1);
}

} // namespace novaboot::http
