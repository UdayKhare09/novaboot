#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <type_traits>
#include <utility>
#include <meta>
#include <simdjson.h>

namespace novaboot::json {

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
    if constexpr (std::is_same_v<T, std::string>) {
        std::string_view sv;
        if (elem.get(sv) == simdjson::SUCCESS) {
            obj = std::string(sv);
        }
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        std::string_view sv;
        if (elem.get(sv) == simdjson::SUCCESS) {
            obj = sv;
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        bool val;
        if (elem.get(val) == simdjson::SUCCESS) {
            obj = val;
        }
    } else if constexpr (std::is_integral_v<T>) {
        int64_t val;
        if (elem.get(val) == simdjson::SUCCESS) {
            obj = static_cast<T>(val);
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        double val;
        if (elem.get(val) == simdjson::SUCCESS) {
            obj = static_cast<T>(val);
        }
    } else if constexpr (std::is_enum_v<T>) {
        std::string_view sv;
        if (elem.get(sv) == simdjson::SUCCESS) {
            static constexpr auto enums = detail::get_enumerator_list<T>();
            template for (constexpr auto e : enums) {
                if (std::meta::identifier_of(e) == sv) {
                    obj = [:e:];
                }
            }
        } else {
            int64_t val;
            if (elem.get(val) == simdjson::SUCCESS) {
                obj = static_cast<T>(val);
            }
        }
    } else if constexpr (is_vector<T>::value) {
        simdjson::dom::array arr;
        if (elem.get(arr) == simdjson::SUCCESS) {
            obj.clear();
            for (auto item : arr) {
                typename T::value_type val{};
                deserialize_elem(item, val);
                obj.push_back(std::move(val));
            }
        }
    } else {
#ifdef __cpp_impl_reflection
        static constexpr auto members = detail::get_members<T>();
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                constexpr auto name = std::meta::identifier_of(m);
                simdjson::dom::element field;
                if (elem[name].get(field) == simdjson::SUCCESS) {
                    deserialize_elem(field, obj.*&[:m:]);
                }
            }
        }
#endif
    }
}

/// Main entry point for JSON deserialization
template<typename T>
T deserialize(std::string_view json_str) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(json_str);
    T obj{};
    if (doc.error() == simdjson::SUCCESS) {
        deserialize_elem(doc.value(), obj);
    }
    return obj;
}

} // namespace novaboot::json
