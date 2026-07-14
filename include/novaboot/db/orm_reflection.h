#pragma once
#include <meta>
#include <string>
#include <string_view>
#include <concepts>
#include <cctype>
#include "novaboot/annotations/stereotypes.h"
#include "novaboot/di/container.h"
#include "novaboot/db/db_client.h"
#include "novaboot/db/uuid.h"
#include <chrono>

namespace novaboot::db::detail {

// ---------------------------------------------------------------------------
// Fixed-size compile-time member list
// ---------------------------------------------------------------------------
struct MemberList {
    std::meta::info data[128] = {};
    std::size_t size = 0;
    consteval const std::meta::info* begin() const noexcept { return data; }
    consteval const std::meta::info* end()   const noexcept { return data + size; }
};

template<typename T>
consteval MemberList get_members() {
    constexpr auto ctx = std::meta::access_context::current();
    MemberList result;
    for (auto m : std::meta::members_of(std::meta::dealias(^^T), ctx)) {
        if (result.size < 128) result.data[result.size++] = m;
    }
    return result;
}

// Fixed-size list of enumerators for @Enumerated(String) mapping
template<typename Enum>
consteval MemberList get_enumerators() {
    MemberList result;
    for (auto e : std::meta::enumerators_of(^^Enum)) {
        if (result.size < 128) result.data[result.size++] = e;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline std::string to_snake_case(std::string_view s) {
    std::string result;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (std::isupper(c)) {
            if (i > 0 && s[i-1] != '_') result += '_';
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
        while (i < sv.length() && i < 63) { name[i] = sv[i]; i++; }
        name[i] = '\0';
    }
};

template<std::meta::info Member>
consteval ColumnInfo get_member_column_name() {
    using namespace novaboot::annotations;
    if constexpr (novaboot::di::detail::has_annotation<Column>(Member)) {
        constexpr auto col = novaboot::di::detail::get_annotation<Column>(Member);
        if (col.name[0] != '\0') return ColumnInfo(std::string_view(col.name));
    }
    constexpr auto raw_name = std::meta::identifier_of(Member);
    return ColumnInfo(raw_name);
}

template<typename Class, auto FieldPtr>
consteval ColumnInfo get_column_name() {
    static constexpr auto members = get_members<Class>();
    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            if constexpr (std::is_same_v<decltype(&[:m:]), decltype(FieldPtr)>) {
                if constexpr (&[:m:] == FieldPtr) return get_member_column_name<m>();
            }
        }
    }
    return ColumnInfo{};
}

template<typename T>
consteval novaboot::annotations::Entity get_table_metadata() {
    using namespace novaboot::annotations;
    if constexpr (novaboot::di::detail::has_annotation<Entity>(^^T))
        return novaboot::di::detail::get_annotation<Entity>(^^T);
    return Entity{};
}

// ---------------------------------------------------------------------------
// Lifecycle hook invocation — calls every member function annotated with Ann
// GCC 16: detect member functions via is_function + is_class_member + !is_static_member
// ---------------------------------------------------------------------------
template<typename Ann, typename T>
void invoke_lifecycle(T& entity) {
    static constexpr auto members = get_members<T>();
    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_function(m) &&
                      std::meta::is_class_member(m) &&
                      !std::meta::is_static_member(m)) {
            if constexpr (novaboot::di::detail::has_annotation<Ann>(m)) {
                entity.[:m:]();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// map_row_to_entity — SELECT result → struct
//
//  @Transient  — skip field entirely (no col_idx increment)
//  @Enumerated — read as string (name lookup) or int (cast)
//  @PostLoad   — call annotated member functions after all fields populated
// ---------------------------------------------------------------------------
template<typename T>
T map_row_to_entity(ResultSet* rs) {
    T entity{};
    static constexpr auto members = get_members<T>();
    int col_idx = 0;

    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            using FT = std::remove_cvref_t<decltype(entity.[:m:])>;

            // @Transient — no column in the result set
            if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Transient>(m)) {
                // intentionally skip — no col_idx++
            } else if (!rs->is_null(col_idx)) {
                if constexpr (std::is_enum_v<FT>) {
                    // @Enumerated
                    if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Enumerated>(m)) {
                        constexpr auto en = novaboot::di::detail::get_annotation<novaboot::annotations::Enumerated>(m);
                        if constexpr (en.value == novaboot::annotations::EnumType::Ordinal) {
                            entity.[:m:] = static_cast<FT>(rs->get_int(col_idx));
                        } else {
                            std::string stored = rs->get_string(col_idx);
                            static constexpr auto enumerators = get_enumerators<FT>();
                            template for (constexpr auto e : enumerators) {
                                if (std::meta::identifier_of(e) == stored) entity.[:m:] = [:e:];
                            }
                        }
                    } else {
                        entity.[:m:] = static_cast<FT>(rs->get_int(col_idx));
                    }
                    col_idx++;
                } else if constexpr (std::is_same_v<FT, int> || std::is_same_v<FT, std::int64_t>) {
                    entity.[:m:] = static_cast<FT>(rs->get_int(col_idx)); col_idx++;
                } else if constexpr (std::is_same_v<FT, double> || std::is_same_v<FT, float>) {
                    entity.[:m:] = static_cast<FT>(rs->get_double(col_idx)); col_idx++;
                } else if constexpr (std::is_same_v<FT, std::string>) {
                    entity.[:m:] = rs->get_string(col_idx); col_idx++;
                } else if constexpr (std::is_same_v<FT, bool>) {
                    entity.[:m:] = rs->get_bool(col_idx); col_idx++;
                } else if constexpr (std::is_same_v<FT, std::vector<std::uint8_t>>) {
                    entity.[:m:] = rs->get_blob(col_idx); col_idx++;
                } else if constexpr (std::is_same_v<FT, Uuid>) {
                    entity.[:m:] = rs->get_uuid(col_idx); col_idx++;
                } else if constexpr (std::is_same_v<FT, std::chrono::system_clock::time_point>) {
                    entity.[:m:] = rs->get_time(col_idx); col_idx++;
                } else {
                    col_idx++;
                }
            } else {
                col_idx++;
            }
        }
    }

    invoke_lifecycle<novaboot::annotations::PostLoad>(entity);
    return entity;
}

} // namespace novaboot::db::detail
