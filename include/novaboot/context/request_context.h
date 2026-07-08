#pragma once

#include <cstdint>
#include <any>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>

namespace novaboot::context {

/// Per-request context for dependency injection and middleware data passing.
///
/// Middleware can store typed values (e.g., auth info, parsed JWT,
/// database connection) and downstream handlers can retrieve them.
///
/// Usage:
///   // In auth middleware:
///   ctx.set<UserId>(user_id);
///
///   // In route handler:
///   auto user_id = ctx.get<UserId>();
class RequestContext {
public:
    RequestContext() = default;

    /// Store a value by type
    template <typename T>
    void set(T value) {
        values_[std::type_index(typeid(T))] = std::move(value);
    }

    /// Retrieve a value by type. Returns nullptr if not found.
    template <typename T>
    [[nodiscard]] T* get() {
        auto it = values_.find(std::type_index(typeid(T)));
        if (it == values_.end()) return nullptr;
        return std::any_cast<T>(&it->second);
    }

    /// Retrieve a value by type (const). Returns nullptr if not found.
    template <typename T>
    [[nodiscard]] const T* get() const {
        auto it = values_.find(std::type_index(typeid(T)));
        if (it == values_.end()) return nullptr;
        return std::any_cast<T>(&it->second);
    }

    /// Check if a value of type T exists
    template <typename T>
    [[nodiscard]] bool has() const {
        return values_.contains(std::type_index(typeid(T)));
    }

    /// Remove a value by type
    template <typename T>
    void remove() {
        values_.erase(std::type_index(typeid(T)));
    }

    /// Store a named string value (for simple key-value data)
    void set_string(std::string_view key, std::string value) {
        string_values_[std::string(key)] = std::move(value);
    }

    /// Get a named string value
    [[nodiscard]] std::string_view
    get_string(std::string_view key) const {
        auto it = string_values_.find(std::string(key));
        if (it != string_values_.end()) {
            return it->second;
        }
        return {};
    }

    /// Clear all context data
    void clear() {
        values_.clear();
        string_values_.clear();
    }

private:
    std::unordered_map<std::type_index, std::any> values_;
    std::unordered_map<std::string, std::string>  string_values_;
};

} // namespace novaboot::context
