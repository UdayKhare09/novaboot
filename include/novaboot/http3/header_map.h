#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace novaboot::http3 {

/// Case-insensitive header storage with multi-value support.
///
/// Optimized for the common case of few headers with short values.
/// Uses a flat vector (cache-friendly for small counts).
class HeaderMap {
public:
    struct Entry {
        std::string name;
        std::string value;
    };

    HeaderMap() = default;

    /// Add a header (allows duplicate names for multi-value headers)
    void add(std::string_view name, std::string_view value);

    /// Set a header (replaces existing if present, adds if not)
    void set(std::string_view name, std::string_view value);

    /// Get the first value for a header name (case-insensitive)
    [[nodiscard]] std::optional<std::string_view>
    get(std::string_view name) const;

    /// Get all values for a header name
    [[nodiscard]] std::vector<std::string_view>
    get_all(std::string_view name) const;

    /// Check if a header exists
    [[nodiscard]] bool has(std::string_view name) const;

    /// Remove all values for a header name
    void remove(std::string_view name);

    /// Get all entries
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept {
        return entries_;
    }

    /// Number of headers
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    /// Check if empty
    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty();
    }

    /// Clear all headers
    void clear() noexcept { entries_.clear(); }

    /// Reserve space for n headers
    void reserve(std::size_t n) { entries_.reserve(n); }

private:
    /// Case-insensitive comparison
    static bool names_equal(std::string_view a, std::string_view b);

    std::vector<Entry> entries_;
};

} // namespace novaboot::http3
