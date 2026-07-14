#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace novaboot::router {

/// Extracted path parameters from URL routing.
///
/// Example: route "/users/:id/posts/:post_id" matched against
///          "/users/42/posts/123" yields { "id":"42", "post_id":"123" }
class PathParams {
public:
    PathParams() = default;

    /// Set a path parameter
    void set(std::string_view name, std::string_view value) {
        params_[std::string(name)] = std::string(value);
    }

    /// Get a path parameter as string
    [[nodiscard]] std::optional<std::string_view>
    get(std::string_view name) const {
        auto it = params_.find(std::string(name));
        if (it != params_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// Get a path parameter with type conversion
    template <typename T>
    [[nodiscard]] std::optional<T> get_as(std::string_view name) const {
        auto val = get(name);
        if (!val) return std::nullopt;

        if constexpr (std::is_same_v<T, std::string>) {
            return std::string(*val);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return *val;
        } else {
            T result;
            auto [ptr, ec] = std::from_chars(
                val->data(), val->data() + val->size(), result);
            if (ec != std::errc{}) return std::nullopt;
            return result;
        }
    }

    /// Check if a parameter exists
    [[nodiscard]] bool has(std::string_view name) const {
        return params_.contains(std::string(name));
    }

    /// Number of parameters
    [[nodiscard]] std::size_t size() const noexcept {
        return params_.size();
    }

    /// Clear all parameters
    void clear() noexcept { params_.clear(); }

private:
    std::unordered_map<std::string, std::string> params_;
};

} // namespace novaboot::router
