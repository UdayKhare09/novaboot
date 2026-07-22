#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <type_traits>
#include <utility>
#include <stdexcept>
#include <meta>
#include <simdjson.h>

namespace novaboot::json {

/// A client-safe JSON binding failure. `errors()` contains JSON paths such as
/// `$.settings.levels[1]: expected integer`, suitable for a 400 response.
class BindingException : public std::invalid_argument {
public:
    explicit BindingException(std::vector<std::string> errors)
        : std::invalid_argument("Invalid JSON request body"), errors_(std::move(errors)) {}

    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

private:
    std::vector<std::string> errors_;
};

namespace detail {
#ifdef __cpp_impl_reflection
template<typename T>
consteval auto get_members() {
    constexpr auto ctx = std::meta::access_context::current();
    struct ArrayWrapper {
        std::meta::info data[64] = {};
        std::size_t     size = 0;

        consteval const std::meta::info* begin() const noexcept { return data; }
        consteval const std::meta::info* end() const noexcept { return data + size; }
    };
    ArrayWrapper result;
    for (auto m : std::meta::members_of(^^T, ctx)) {
        if (result.size < 64) {
            result.data[result.size++] = m;
        }
    }
    return result;
}
template<typename Enum>
consteval auto get_enumerator_list() {
    struct ArrayWrapper {
        std::meta::info data[64] = {};
        std::size_t     size = 0;

        consteval const std::meta::info* begin() const noexcept { return data; }
        consteval const std::meta::info* end() const noexcept { return data + size; }
    };
    ArrayWrapper result;
    for (auto e : std::meta::enumerators_of(^^Enum)) {
        if (result.size < 64) {
            result.data[result.size++] = e;
        }
    }
    return result;
}
#endif
} // namespace detail

// Forward declaration of is_vector helper
template<typename T>
struct is_vector : std::false_type {};

template<typename T, typename Alloc>
struct is_vector<std::vector<T, Alloc>> : std::true_type {};

// Forward declarations
template<typename T>
std::string serialize(const T& obj);

template<typename T>
void deserialize_elem(simdjson::dom::element elem, T& obj);

namespace detail {

[[noreturn]] inline void binding_error(std::string_view path, std::string_view expected) {
    throw BindingException({std::string(path) + ": expected " + std::string(expected)});
}

template<typename T>
void deserialize_elem_at(simdjson::dom::element elem, T& obj, const std::string& path);

} // namespace detail

/// Reflection-based JSON Serializer
template<typename T>
std::string serialize(const T& obj) {
    if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
        return "\"" + std::string(obj) + "\"";
    } else if constexpr (std::is_same_v<T, const char*>) {
        return "\"" + std::string(obj) + "\"";
    } else if constexpr (std::is_same_v<T, bool>) {
        return obj ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(obj);
    } else if constexpr (std::is_enum_v<T>) {
        std::string enum_str = "\"";
        static constexpr auto enums = detail::get_enumerator_list<T>();
        bool matched = false;
        template for (constexpr auto e : enums) {
            if ([:e:] == obj) {
                enum_str += std::meta::identifier_of(e);
                matched = true;
            }
        }
        if (!matched) {
            enum_str += std::to_string(static_cast<std::int64_t>(obj));
        }
        enum_str += "\"";
        return enum_str;
    } else if constexpr (is_vector<T>::value) {
        std::string out = "[";
        bool first = true;
        for (const auto& item : obj) {
            if (!first) out += ",";
            first = false;
            out += serialize(item);
        }
        out += "]";
        return out;
    } else {
#ifdef __cpp_impl_reflection
        std::string out = "{";
        static constexpr auto members = detail::get_members<T>();
        bool first = true;
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                if (!first) out += ",";
                first = false;
                constexpr auto name = std::meta::identifier_of(m);
                out += "\"" + std::string(name) + "\":";
                out += serialize(obj.*&[:m:]);
            }
        }
        out += "}";
        return out;
#else
        return "{}";
#endif
    }
}

/// Reflection-based JSON Deserializer elements mapper
template<typename T>
void deserialize_elem(simdjson::dom::element elem, T& obj) {
    detail::deserialize_elem_at(elem, obj, "$");
}

template<typename T>
void detail::deserialize_elem_at(simdjson::dom::element elem, T& obj,
                                 const std::string& path) {
    if constexpr (std::is_same_v<T, std::string>) {
        std::string_view sv;
        if (elem.get(sv) == simdjson::SUCCESS) {
            obj = std::string(sv);
        } else {
            binding_error(path, "string");
        }
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        std::string_view sv;
        if (elem.get(sv) == simdjson::SUCCESS) {
            obj = sv;
        } else {
            binding_error(path, "string");
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        bool val;
        if (elem.get(val) == simdjson::SUCCESS) {
            obj = val;
        } else {
            binding_error(path, "boolean");
        }
    } else if constexpr (std::is_integral_v<T>) {
        int64_t val;
        if (elem.get(val) == simdjson::SUCCESS) {
            obj = static_cast<T>(val);
        } else {
            binding_error(path, "integer");
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        double val;
        if (elem.get(val) == simdjson::SUCCESS) {
            obj = static_cast<T>(val);
        } else {
            binding_error(path, "number");
        }
    } else if constexpr (std::is_enum_v<T>) {
        std::string_view sv;
        if (elem.get(sv) == simdjson::SUCCESS) {
            static constexpr auto enums = detail::get_enumerator_list<T>();
            bool matched = false;
            template for (constexpr auto e : enums) {
                if (std::meta::identifier_of(e) == sv) {
                    obj = [:e:];
                    matched = true;
                }
            }
            if (!matched) binding_error(path, "known enum value");
        } else {
            int64_t val;
            if (elem.get(val) == simdjson::SUCCESS) {
                obj = static_cast<T>(val);
            } else {
                binding_error(path, "enum string or ordinal");
            }
        }
    } else if constexpr (is_vector<T>::value) {
        simdjson::dom::array arr;
        if (elem.get(arr) == simdjson::SUCCESS) {
            obj.clear();
            std::size_t index = 0;
            for (auto item : arr) {
                typename T::value_type val{};
                deserialize_elem_at(item, val, path + "[" + std::to_string(index++) + "]");
                obj.push_back(std::move(val));
            }
        } else {
            binding_error(path, "array");
        }
    } else {
#ifdef __cpp_impl_reflection
        simdjson::dom::object object;
        if (elem.get(object) != simdjson::SUCCESS) {
            binding_error(path, "object");
        }
        static constexpr auto members = detail::get_members<T>();
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                constexpr auto name = std::meta::identifier_of(m);
                simdjson::dom::element field;
                if (object[name].get(field) == simdjson::SUCCESS) {
                    deserialize_elem_at(field, obj.*&[:m:],
                                        path + "." + std::string(name));
                }
            }
        }
#else
        binding_error(path, "supported JSON value");
#endif
    }
}

/// Main entry point for JSON deserialization
template<typename T>
T deserialize(std::string_view json_str) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(json_str);
    T obj{};
    if (doc.error() != simdjson::SUCCESS) {
        throw BindingException({"$: malformed JSON"});
    }
    deserialize_elem(doc.value(), obj);
    return obj;
}

} // namespace novaboot::json
