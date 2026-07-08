#include "novaboot/http3/header_map.h"

#include <algorithm>
#include <cctype>

namespace novaboot::http3 {

bool HeaderMap::names_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

void HeaderMap::add(std::string_view name, std::string_view value) {
    entries_.push_back({std::string(name), std::string(value)});
}

void HeaderMap::set(std::string_view name, std::string_view value) {
    for (auto& entry : entries_) {
        if (names_equal(entry.name, name)) {
            entry.value = std::string(value);
            return;
        }
    }
    add(name, value);
}

std::optional<std::string_view>
HeaderMap::get(std::string_view name) const {
    for (const auto& entry : entries_) {
        if (names_equal(entry.name, name)) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::vector<std::string_view>
HeaderMap::get_all(std::string_view name) const {
    std::vector<std::string_view> result;
    for (const auto& entry : entries_) {
        if (names_equal(entry.name, name)) {
            result.push_back(entry.value);
        }
    }
    return result;
}

bool HeaderMap::has(std::string_view name) const {
    return get(name).has_value();
}

void HeaderMap::remove(std::string_view name) {
    std::erase_if(entries_, [&name](const Entry& entry) {
        return names_equal(entry.name, name);
    });
}

} // namespace novaboot::http3
