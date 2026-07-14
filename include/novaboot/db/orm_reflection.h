#pragma once
#include <meta>
#include <string>
#include <string_view>
#include <concepts>
#include <cctype>
#include "novaboot/annotations/stereotypes.h"
#include "novaboot/di/container.h"
#include "novaboot/db/db_client.h"

namespace novaboot::db::detail {

struct MemberList {
    std::meta::info data[128] = {};
    std::size_t size = 0;
    
    consteval const std::meta::info* begin() const noexcept { return data; }
    consteval const std::meta::info* end() const noexcept { return data + size; }
};

template<typename T>
consteval MemberList get_members() {
    constexpr auto ctx = std::meta::access_context::current();
    MemberList result;
    for (auto m : std::meta::members_of(std::meta::dealias(^^T), ctx)) {
        if (result.size < 128) {
            result.data[result.size++] = m;
        }
    }
    return result;
}

/// Helper to convert CamelCase string to snake_case
inline std::string to_snake_case(std::string_view s) {
    std::string result;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (std::isupper(c)) {
            if (i > 0 && s[i-1] != '_') {
                result += '_';
            }
            result += static_cast<char>(std::tolower(c));
        } else {
            result += c;
        }
    }
    return result;
}

struct ColumnInfo {
    char name[64] = {};
    consteval ColumnInfo() = default;
    consteval ColumnInfo(std::string_view sv) {
        size_t i = 0;
        while (i < sv.length() && i < 63) {
            name[i] = sv[i];
            i++;
        }
        name[i] = '\0';
    }
};

/// Compile-time extraction of column name for a field member metadata
template<std::meta::info Member>
consteval ColumnInfo get_member_column_name() {
    using namespace novaboot::annotations;
    if constexpr (novaboot::di::detail::has_annotation<Column>(Member)) {
        constexpr auto col = novaboot::di::detail::get_annotation<Column>(Member);
        if (col.name[0] != '\0') {
            return ColumnInfo(std::string_view(col.name));
        }
    }
    // Convert field variable identifier to default snake_case column
    constexpr auto raw_name = std::meta::identifier_of(Member);
    return ColumnInfo(raw_name);
}

/// Compile-time resolution of a member pointer to its column name
template<typename Class, auto FieldPtr>
consteval ColumnInfo get_column_name() {
    static constexpr auto members = get_members<Class>();
    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            if constexpr (std::is_same_v<decltype(&[:m:]), decltype(FieldPtr)>) {
                if constexpr (&[:m:] == FieldPtr) {
                    return get_member_column_name<m>();
                }
            }
        }
    }
    return ColumnInfo{};
}

/// Compile-time table name resolution returning Entity metadata
template<typename T>
consteval novaboot::annotations::Entity get_table_metadata() {
    using namespace novaboot::annotations;
    if constexpr (novaboot::di::detail::has_annotation<Entity>(^^T)) {
        return novaboot::di::detail::get_annotation<Entity>(^^T);
    }
    return Entity{};
}

/// Runtime mapping from ResultSet columns back to reflected struct fields
template<typename T>
T map_row_to_entity(ResultSet* rs) {
    T entity{};
    static constexpr auto members = get_members<T>();
    int col_idx = 0;
    
    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            using FieldType = decltype(entity.[:m:]);
            
            if (!rs->is_null(col_idx)) {
                if constexpr (std::is_same_v<FieldType, int> || 
                              std::is_same_v<FieldType, std::int64_t>) {
                    entity.[:m:] = static_cast<FieldType>(rs->get_int(col_idx));
                } else if constexpr (std::is_same_v<FieldType, double> ||
                                     std::is_same_v<FieldType, float>) {
                    entity.[:m:] = static_cast<FieldType>(rs->get_double(col_idx));
                } else if constexpr (std::is_same_v<FieldType, std::string>) {
                    entity.[:m:] = rs->get_string(col_idx);
                } else if constexpr (std::is_same_v<FieldType, bool>) {
                    entity.[:m:] = rs->get_bool(col_idx);
                } else if constexpr (std::is_same_v<FieldType, std::vector<std::uint8_t>>) {
                    entity.[:m:] = rs->get_blob(col_idx);
                }
            }
            col_idx++;
        }
    }
    return entity;
}

} // namespace novaboot::db::detail
