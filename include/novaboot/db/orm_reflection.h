#pragma once
#include <meta>
#include <string>
#include <string_view>
#include <concepts>
#include <cctype>
#include "novaboot/annotations/stereotypes.h"
#include "novaboot/di/container.h"
#include "novaboot/db/db_client.h"
#include "novaboot/db/lazy.h"
#include "novaboot/db/uuid.h"
#include "novaboot/router/json.h"
#include <chrono>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace novaboot::db::detail {

struct EntityLoadContext {
    DataSource* datasource = nullptr;
    std::shared_ptr<Connection> connection;
    std::unordered_set<std::string> loading;
    std::unordered_set<std::string> fetch_members;
    std::unordered_map<std::string, std::string> joined_fetch_prefixes;
    std::string column_prefix;
    bool retain_connection_for_lazy = false;
};

template<typename T>
struct is_std_vector : std::false_type {};

template<typename Value, typename Allocator>
struct is_std_vector<std::vector<Value, Allocator>> : std::true_type {};

template<typename T>
struct is_lazy_collection : std::false_type {};

template<typename Value>
struct is_lazy_collection<novaboot::db::LazyCollection<Value>> : std::true_type {};

template<typename T>
struct vector_value_type {
    using type = void;
};

template<typename Value, typename Allocator>
struct vector_value_type<std::vector<Value, Allocator>> {
    using type = Value;
};

template<typename Value>
struct vector_value_type<novaboot::db::LazyCollection<Value>> {
    using type = Value;
};

template<typename T>
inline constexpr bool is_collection_relation_v =
    is_std_vector<T>::value || is_lazy_collection<T>::value;

template<typename T>
struct is_lazy_relation : std::false_type {};

template<typename T>
struct is_lazy_relation<novaboot::db::Lazy<T>> : std::true_type {};

template<typename T>
struct is_optional_relation : std::false_type {};

template<typename T>
struct is_optional_relation<std::optional<T>> : std::true_type {};

template<typename T>
struct relation_value_type {
    using type = T;
};

template<typename T>
struct relation_value_type<novaboot::db::Lazy<T>> {
    using type = T;
};

template<typename T>
struct relation_value_type<std::optional<T>> {
    using type = T;
};

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

template<typename Entity>
consteval std::meta::info entity_id_member() {
    static constexpr auto members = get_members<Entity>();
    template for (constexpr auto member : members) {
        if constexpr (std::meta::is_nonstatic_data_member(member) &&
                      novaboot::di::detail::has_annotation<novaboot::annotations::Id>(member)) {
            return member;
        }
    }
    return ^^void;
}

template<typename Entity>
struct entity_id_type {
    static constexpr auto member = entity_id_member<Entity>();
    static_assert(member != ^^void, "Entity has no @Id field");
    using type = std::remove_cvref_t<decltype(std::declval<Entity&>().[:member:])>;
};

// Fixed-size list of enumerators for @Enumerated(String) mapping
template<typename Enum>
consteval MemberList get_enumerators() {
    MemberList result;
    for (auto e : std::meta::enumerators_of(^^Enum)) {
        if (result.size < 128) result.data[result.size++] = e;
    }
    return result;
}

template<typename Enum>
std::string enum_name(Enum value) {
    static constexpr auto enumerators = get_enumerators<Enum>();
    template for (constexpr auto e : enumerators) {
        if (value == [:e:]) {
            return std::string(std::meta::identifier_of(e));
        }
    }
    return std::to_string(static_cast<std::int64_t>(value));
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
    if constexpr (novaboot::di::detail::has_annotation<JoinColumn>(Member)) {
        constexpr auto join = novaboot::di::detail::get_annotation<JoinColumn>(Member);
        if constexpr (join.name[0] != '\0') return ColumnInfo(std::string_view(join.name));
    }
    if constexpr (novaboot::di::detail::has_annotation<Column>(Member)) {
        constexpr auto col = novaboot::di::detail::get_annotation<Column>(Member);
        if (col.name[0] != '\0') return ColumnInfo(std::string_view(col.name));
    }
    constexpr auto raw_name = std::meta::identifier_of(Member);
    return ColumnInfo(raw_name);
}

template<std::meta::info Member>
consteval bool is_persisted_entity_member() {
    using namespace novaboot::annotations;
    if constexpr (!std::meta::is_nonstatic_data_member(Member)) return false;
    if constexpr (novaboot::di::detail::has_annotation<Transient>(Member)) return false;
    if constexpr (novaboot::di::detail::has_annotation<OneToMany>(Member)) return false;
    if constexpr (novaboot::di::detail::has_annotation<ManyToMany>(Member)) return false;
    if constexpr (novaboot::di::detail::has_annotation<OneToOne>(Member)) return false;
    return true;
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

template<typename Class, auto FieldPtr>
consteval std::meta::info get_member_info_for_field() {
    static constexpr auto members = get_members<Class>();
    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            if constexpr (std::is_same_v<decltype(&[:m:]), decltype(FieldPtr)>) {
                if constexpr (&[:m:] == FieldPtr) return m;
            }
        }
    }
    return std::meta::info{};
}

template<typename T>
consteval novaboot::annotations::Entity get_table_metadata() {
    using namespace novaboot::annotations;
    if constexpr (novaboot::di::detail::has_annotation<Entity>(^^T))
        return novaboot::di::detail::get_annotation<Entity>(^^T);
    return Entity{};
}

/// Return the persisted entity columns in declaration order.  Repository
/// queries use this rather than SELECT * so database column order cannot alter
/// how an entity is read.
template<typename T>
std::string get_select_column_list() {
    static constexpr auto members = get_members<T>();
    std::string columns;

    template for (constexpr auto m : members) {
        if constexpr (is_persisted_entity_member<m>()) {
            if (!columns.empty()) columns += ", ";
            columns += std::string(get_member_column_name<m>().name);
        }
    }
    return columns;
}

template<typename T>
std::string get_select_column_list(std::string_view qualifier) {
    static constexpr auto members = get_members<T>();
    std::string columns;

    template for (constexpr auto m : members) {
        if constexpr (is_persisted_entity_member<m>()) {
            if (!columns.empty()) columns += ", ";
            if (!qualifier.empty()) {
                columns += std::string(qualifier) + ".";
            }
            columns += std::string(get_member_column_name<m>().name);
        }
    }
    return columns;
}

template<typename T>
std::string get_select_column_list_with_aliases(std::string_view qualifier,
                                                std::string_view alias_prefix) {
    static constexpr auto members = get_members<T>();
    std::string columns;

    template for (constexpr auto m : members) {
        if constexpr (is_persisted_entity_member<m>()) {
            constexpr auto column = get_member_column_name<m>();
            if (!columns.empty()) columns += ", ";
            if (!qualifier.empty()) {
                columns += std::string(qualifier) + ".";
            }
            columns += std::string(column.name) + " AS " +
                       std::string(alias_prefix) + std::string(column.name);
        }
    }
    return columns;
}

template<typename T>
bool is_persisted_column(std::string_view column_name) {
    static constexpr auto members = get_members<T>();
    template for (constexpr auto m : members) {
        if constexpr (is_persisted_entity_member<m>()) {
            constexpr auto column = get_member_column_name<m>();
            if (column_name == column.name) return true;
        }
    }
    return false;
}

inline int find_column_index(ResultSet* rs, std::string_view column_name) {
    for (int index = 0; index < rs->column_count(); ++index) {
        if (rs->column_name(index) == column_name) return index;
    }
    return -1;
}

inline std::string prefixed_column_name(EntityLoadContext* context, std::string_view column_name) {
    if (!context || context->column_prefix.empty()) return std::string(column_name);
    return context->column_prefix + std::string(column_name);
}

template<typename Entity>
Parameter entity_id_parameter(const Entity& entity) {
    static constexpr auto members = get_members<Entity>();
    template for (constexpr auto member : members) {
        if constexpr (std::meta::is_nonstatic_data_member(member) &&
                      novaboot::di::detail::has_annotation<novaboot::annotations::Id>(member)) {
            using Id = std::remove_cvref_t<decltype(entity.[:member:])>;
            if constexpr (std::is_integral_v<Id> && !std::is_same_v<Id, bool>) {
                return Parameter(static_cast<std::int64_t>(entity.[:member:]));
            } else if constexpr (std::is_floating_point_v<Id>) {
                return Parameter(static_cast<double>(entity.[:member:]));
            } else {
                return Parameter(entity.[:member:]);
            }
        }
    }
    throw std::invalid_argument("Relationship target has no @Id field");
}

template<typename Entity>
Parameter entity_id_parameter(const novaboot::db::Lazy<Entity>& lazy) {
    if (lazy.id_parameter()) return *lazy.id_parameter();
    return entity_id_parameter(lazy.get());
}

template<typename Entity>
Parameter entity_id_parameter(const std::optional<Entity>& optional) {
    if (!optional) return Parameter(nullptr);
    return entity_id_parameter(*optional);
}

inline std::string temporal_to_string(std::chrono::system_clock::time_point value,
                                      novaboot::annotations::TemporalType temporal_type) {
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm tm = *std::gmtime(&time);
    std::stringstream stream;
    if (temporal_type == novaboot::annotations::TemporalType::Date) {
        stream << std::put_time(&tm, "%Y-%m-%d");
    } else if (temporal_type == novaboot::annotations::TemporalType::Time) {
        stream << std::put_time(&tm, "%H:%M:%S");
    } else {
        stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    }
    return stream.str();
}

inline std::chrono::system_clock::time_point temporal_from_string(
    const std::string& value, novaboot::annotations::TemporalType temporal_type) {
    std::tm tm = {};
    std::stringstream stream(value);
    if (temporal_type == novaboot::annotations::TemporalType::Date) {
        stream >> std::get_time(&tm, "%Y-%m-%d");
    } else if (temporal_type == novaboot::annotations::TemporalType::Time) {
        tm.tm_year = 70;
        tm.tm_mday = 1;
        stream >> std::get_time(&tm, "%H:%M:%S");
    } else if (value.find('T') != std::string::npos) {
        stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    } else {
        stream >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    }
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

template<typename Entity>
Entity entity_with_id(ResultSet* rs, int column_index) {
    Entity entity{};
    static constexpr auto members = get_members<Entity>();
    template for (constexpr auto member : members) {
        if constexpr (std::meta::is_nonstatic_data_member(member) &&
                      novaboot::di::detail::has_annotation<novaboot::annotations::Id>(member)) {
            using Id = std::remove_cvref_t<decltype(entity.[:member:])>;
            if constexpr (std::is_same_v<Id, int> || std::is_same_v<Id, std::int64_t>) {
                entity.[:member:] = static_cast<Id>(rs->get_int(column_index));
            } else if constexpr (std::is_same_v<Id, double> || std::is_same_v<Id, float>) {
                entity.[:member:] = static_cast<Id>(rs->get_double(column_index));
            } else if constexpr (std::is_same_v<Id, std::string>) {
                entity.[:member:] = rs->get_string(column_index);
            } else if constexpr (std::is_same_v<Id, Uuid>) {
                entity.[:member:] = rs->get_uuid(column_index);
            } else {
                static_assert(std::is_same_v<Id, void>, "Unsupported @ManyToOne identifier type");
            }
        }
    }
    return entity;
}

template<typename Entity>
std::string entity_table_name() {
    using namespace novaboot::annotations;
    if constexpr (novaboot::di::detail::has_annotation<Table>(^^Entity)) {
        constexpr auto table = novaboot::di::detail::get_annotation<Table>(^^Entity);
        if constexpr (table.name[0] != '\0') {
            if constexpr (table.schema[0] != '\0') return std::string(table.schema) + "." + table.name;
            return table.name;
        }
    }
    constexpr auto entity = get_table_metadata<Entity>();
    if constexpr (entity.name[0] != '\0') return entity.name;
    constexpr auto raw_name = std::meta::identifier_of(^^Entity);
    return to_snake_case(raw_name) + "s";
}

template<typename Entity>
std::string entity_primary_key_column() {
    static constexpr auto members = get_members<Entity>();
    template for (constexpr auto member : members) {
        if constexpr (std::meta::is_nonstatic_data_member(member) &&
                      novaboot::di::detail::has_annotation<novaboot::annotations::Id>(member)) {
            return std::string(get_member_column_name<member>().name);
        }
    }
    throw std::invalid_argument("Relationship target has no @Id field");
}

template<typename Child>
std::string child_join_column_for_mapped_by(std::string_view mapped_by) {
    static constexpr auto members = get_members<Child>();
    template for (constexpr auto member : members) {
        if constexpr (std::meta::is_nonstatic_data_member(member) &&
                      novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(member)) {
            if (std::meta::identifier_of(member) == mapped_by) {
                return std::string(get_member_column_name<member>().name);
            }
        }
    }
    throw std::invalid_argument("@OneToMany mapped_by does not name a @ManyToOne field");
}

template<typename Child, typename Parent>
void set_many_to_one_reference(Child& child, std::string_view mapped_by, const Parent& parent) {
    static constexpr auto members = get_members<Child>();
    bool matched = false;
    template for (constexpr auto member : members) {
        if constexpr (std::meta::is_nonstatic_data_member(member) &&
                      novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(member)) {
            if (std::meta::identifier_of(member) == mapped_by) {
                using Field = std::remove_cvref_t<decltype(child.[:member:])>;
                if constexpr (is_lazy_relation<Field>::value) {
                    child.[:member:] = Field::loaded(parent, entity_id_parameter(parent));
                } else if constexpr (is_optional_relation<Field>::value) {
                    child.[:member:] = parent;
                } else {
                    child.[:member:] = parent;
                }
                matched = true;
            }
        }
    }
    if (!matched) {
        throw std::invalid_argument("@OneToMany mapped_by does not name a @ManyToOne field");
    }
}

template<typename T>
T map_row_to_entity(ResultSet* rs, EntityLoadContext* context = nullptr);

inline bool should_fetch_member(EntityLoadContext* context, std::string_view member_name) {
    return context && context->fetch_members.contains(std::string(member_name));
}

inline std::optional<std::string> joined_fetch_prefix(EntityLoadContext* context,
                                                      std::string_view member_name) {
    if (!context) return std::nullopt;
    auto it = context->joined_fetch_prefixes.find(std::string(member_name));
    if (it == context->joined_fetch_prefixes.end()) return std::nullopt;
    return it->second;
}

template<typename Entity>
Entity eager_load_many_to_one(ResultSet* rs, int column_index, EntityLoadContext* context) {
    auto identity = entity_with_id<Entity>(rs, column_index);
    if (!context || !context->datasource) return identity;

    const auto table = entity_table_name<Entity>();
    const auto key = table + ":" + format_parameter(entity_id_parameter(identity));
    if (!context->loading.insert(key).second) return identity;

    try {
        const auto sql = "SELECT " + get_select_column_list<Entity>() + " FROM " + table +
                         " WHERE " + entity_primary_key_column<Entity>() + " = ?";
        auto dialect = context->datasource->dialect();
        auto connection = context->connection ? context->connection : context->datasource->get_connection();
        auto result = connection->query(dialect->convert_placeholders(sql),
                                        {entity_id_parameter(identity)});
        if (result->next()) {
            auto related = map_row_to_entity<Entity>(result.get(), context);
            context->loading.erase(key);
            return related;
        }
    } catch (...) {
        context->loading.erase(key);
        throw;
    }

    context->loading.erase(key);
    return identity;
}

template<typename Entity>
novaboot::db::Lazy<Entity> lazy_load_many_to_one(ResultSet* rs, int column_index,
                                                 EntityLoadContext* context) {
    auto identity = entity_with_id<Entity>(rs, column_index);
    auto id = entity_id_parameter(identity);
    if (!context || !context->datasource) {
        return novaboot::db::Lazy<Entity>::loaded(std::move(identity), std::move(id));
    }

    auto* datasource = context->datasource;
    auto retained_connection = context->retain_connection_for_lazy ? context->connection : nullptr;
    return novaboot::db::Lazy<Entity>::unloaded(id, [datasource, retained_connection, identity]() mutable {
        EntityLoadContext load_context;
        load_context.datasource = datasource;
        load_context.connection = retained_connection ? retained_connection : datasource->get_connection();
        load_context.retain_connection_for_lazy = retained_connection != nullptr;

        const auto table = entity_table_name<Entity>();
        const auto sql = "SELECT " + get_select_column_list<Entity>() + " FROM " + table +
                         " WHERE " + entity_primary_key_column<Entity>() + " = ?";
        auto dialect = datasource->dialect();
        auto result = load_context.connection->query(dialect->convert_placeholders(sql),
                                                     {entity_id_parameter(identity)});
        if (result->next()) {
            return map_row_to_entity<Entity>(result.get(), &load_context);
        }
        return identity;
    });
}

template<typename Parent, typename Child>
std::vector<Child> eager_load_one_to_many(const Parent& parent, std::string_view mapped_by,
                                          EntityLoadContext* context) {
    std::vector<Child> children;
    if (!context || !context->datasource) return children;

    const auto parent_table = entity_table_name<Parent>();
    const auto parent_key = parent_table + ":" + format_parameter(entity_id_parameter(parent));
    if (!context->loading.insert(parent_key).second) return children;

    try {
        const auto child_table = entity_table_name<Child>();
        const auto join_column = child_join_column_for_mapped_by<Child>(mapped_by);
        const auto sql = "SELECT " + get_select_column_list<Child>() + " FROM " + child_table +
                         " WHERE " + join_column + " = ?";
        auto dialect = context->datasource->dialect();
        auto connection = context->connection ? context->connection : context->datasource->get_connection();
        auto result = connection->query(dialect->convert_placeholders(sql),
                                        {entity_id_parameter(parent)});
        while (result->next()) {
            children.push_back(map_row_to_entity<Child>(result.get(), context));
        }
    } catch (...) {
        context->loading.erase(parent_key);
        throw;
    }

    context->loading.erase(parent_key);
    return children;
}

template<typename Parent, typename Child>
novaboot::db::LazyCollection<Child> lazy_load_one_to_many(const Parent& parent,
                                                          std::string_view mapped_by,
                                                          EntityLoadContext* context) {
    if (!context || !context->datasource) return {};

    auto* datasource = context->datasource;
    auto retained_connection = context->retain_connection_for_lazy ? context->connection : nullptr;
    const auto parent_id = entity_id_parameter(parent);
    const auto mapped_by_name = std::string(mapped_by);

    auto load = [datasource, retained_connection, parent_id, mapped_by_name]() {
        EntityLoadContext load_context;
        load_context.datasource = datasource;
        load_context.connection = retained_connection ? retained_connection : datasource->get_connection();
        load_context.retain_connection_for_lazy = retained_connection != nullptr;

        const auto parent_table = entity_table_name<Parent>();
        const auto parent_key = parent_table + ":" + format_parameter(parent_id);
        load_context.loading.insert(parent_key);

        const auto child_table = entity_table_name<Child>();
        const auto join_column = child_join_column_for_mapped_by<Child>(mapped_by_name);
        const auto sql = "SELECT " + get_select_column_list<Child>() + " FROM " + child_table +
                         " WHERE " + join_column + " = ?";
        auto dialect = datasource->dialect();
        auto result = load_context.connection->query(dialect->convert_placeholders(sql), {parent_id});

        std::vector<Child> children;
        while (result->next()) {
            children.push_back(map_row_to_entity<Child>(result.get(), &load_context));
        }
        return children;
    };

    auto count = [datasource, retained_connection, parent_id, mapped_by_name]() -> std::int64_t {
        const auto child_table = entity_table_name<Child>();
        const auto join_column = child_join_column_for_mapped_by<Child>(mapped_by_name);
        const auto sql = "SELECT COUNT(1) FROM " + child_table +
                         " WHERE " + join_column + " = ?";
        auto dialect = datasource->dialect();
        auto active_connection = retained_connection ? retained_connection : datasource->get_connection();
        auto result = active_connection->query(dialect->convert_placeholders(sql), {parent_id});
        return result->next() ? result->get_int(0) : 0;
    };

    return novaboot::db::LazyCollection<Child>::unloaded(std::move(load), std::move(count));
}

template<typename Parent, typename Child>
std::vector<Child> eager_load_many_to_many(const Parent& parent,
                                           std::string_view join_table,
                                           std::string_view join_column,
                                           std::string_view inverse_join_column,
                                           EntityLoadContext* context) {
    std::vector<Child> children;
    if (!context || !context->datasource) return children;

    const auto parent_table = entity_table_name<Parent>();
    const auto parent_key = std::string(join_table) + ":" + parent_table + ":" +
                            format_parameter(entity_id_parameter(parent));
    if (!context->loading.insert(parent_key).second) return children;

    try {
        const auto child_table = entity_table_name<Child>();
        const auto child_pk = entity_primary_key_column<Child>();
        const auto sql = "SELECT " + get_select_column_list<Child>("c") +
                         " FROM " + child_table + " c JOIN " + std::string(join_table) +
                         " jt ON c." + child_pk + " = jt." + std::string(inverse_join_column) +
                         " WHERE jt." + std::string(join_column) + " = ?";
        auto dialect = context->datasource->dialect();
        auto connection = context->connection ? context->connection : context->datasource->get_connection();
        auto result = connection->query(dialect->convert_placeholders(sql),
                                        {entity_id_parameter(parent)});
        while (result->next()) {
            children.push_back(map_row_to_entity<Child>(result.get(), context));
        }
    } catch (...) {
        context->loading.erase(parent_key);
        throw;
    }

    context->loading.erase(parent_key);
    return children;
}

template<typename Parent, typename Child>
novaboot::db::LazyCollection<Child> lazy_load_many_to_many(const Parent& parent,
                                                           std::string_view join_table,
                                                           std::string_view join_column,
                                                           std::string_view inverse_join_column,
                                                           EntityLoadContext* context) {
    if (!context || !context->datasource) return {};

    auto* datasource = context->datasource;
    auto retained_connection = context->retain_connection_for_lazy ? context->connection : nullptr;
    const auto parent_id = entity_id_parameter(parent);
    const auto join_table_name = std::string(join_table);
    const auto join_column_name = std::string(join_column);
    const auto inverse_join_column_name = std::string(inverse_join_column);

    auto load = [datasource, retained_connection, parent_id, join_table_name,
                 join_column_name, inverse_join_column_name]() {
        EntityLoadContext load_context;
        load_context.datasource = datasource;
        load_context.connection = retained_connection ? retained_connection : datasource->get_connection();
        load_context.retain_connection_for_lazy = retained_connection != nullptr;

        const auto parent_table = entity_table_name<Parent>();
        const auto parent_key = join_table_name + ":" + parent_table + ":" +
                                format_parameter(parent_id);
        load_context.loading.insert(parent_key);

        const auto child_table = entity_table_name<Child>();
        const auto child_pk = entity_primary_key_column<Child>();
        const auto sql = "SELECT " + get_select_column_list<Child>("c") +
                         " FROM " + child_table + " c JOIN " + join_table_name +
                         " jt ON c." + child_pk + " = jt." + inverse_join_column_name +
                         " WHERE jt." + join_column_name + " = ?";
        auto dialect = datasource->dialect();
        auto result = load_context.connection->query(dialect->convert_placeholders(sql), {parent_id});

        std::vector<Child> children;
        while (result->next()) {
            children.push_back(map_row_to_entity<Child>(result.get(), &load_context));
        }
        return children;
    };

    auto count = [datasource, retained_connection, parent_id, join_table_name, join_column_name]() -> std::int64_t {
        const auto sql = "SELECT COUNT(1) FROM " + join_table_name +
                         " WHERE " + join_column_name + " = ?";
        auto dialect = datasource->dialect();
        auto active_connection = retained_connection ? retained_connection : datasource->get_connection();
        auto result = active_connection->query(dialect->convert_placeholders(sql), {parent_id});
        return result->next() ? result->get_int(0) : 0;
    };

    return novaboot::db::LazyCollection<Child>::unloaded(std::move(load), std::move(count));
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
//  @Transient  — skip field entirely
//  missing column — leave the field's default value intact (enables safe,
//                   future partial projections)
//  @Enumerated — read as string (name lookup) or int (cast)
//  @PostLoad   — call annotated member functions after all fields populated
// ---------------------------------------------------------------------------
template<typename T>
T map_row_to_entity(ResultSet* rs, EntityLoadContext* context) {
    T entity{};
    static constexpr auto members = get_members<T>();

    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            using FT = std::remove_cvref_t<decltype(entity.[:m:])>;

            // @Transient — no column is expected in the result set.
            if constexpr (!is_persisted_entity_member<m>()) {
                // intentionally skip
            } else {
                constexpr auto column = get_member_column_name<m>();
                const int col_idx = find_column_index(rs, prefixed_column_name(context, column.name));
                if (col_idx < 0 || rs->is_null(col_idx)) continue;

                if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(m)) {
                    constexpr auto many_to_one = novaboot::di::detail::get_annotation<novaboot::annotations::ManyToOne>(m);
                    constexpr auto member_name = std::meta::identifier_of(m);
                    if constexpr (is_lazy_relation<FT>::value) {
                        using Target = typename relation_value_type<FT>::type;
                        if (should_fetch_member(context, member_name)) {
                            Target related{};
                            if (auto prefix = joined_fetch_prefix(context, member_name)) {
                                auto joined_context = context ? *context : EntityLoadContext{};
                                joined_context.column_prefix = *prefix;
                                joined_context.fetch_members.clear();
                                joined_context.joined_fetch_prefixes.clear();
                                related = map_row_to_entity<Target>(rs, &joined_context);
                            } else {
                                related = eager_load_many_to_one<Target>(rs, col_idx, context);
                            }
                            auto related_id = entity_id_parameter(related);
                            entity.[:m:] = FT::loaded(std::move(related), std::move(related_id));
                        } else {
                            entity.[:m:] = lazy_load_many_to_one<Target>(rs, col_idx, context);
                        }
                    } else if constexpr (is_optional_relation<FT>::value) {
                        using Target = typename relation_value_type<FT>::type;
                        if (should_fetch_member(context, member_name)) {
                            if (auto prefix = joined_fetch_prefix(context, member_name)) {
                                auto joined_context = context ? *context : EntityLoadContext{};
                                joined_context.column_prefix = *prefix;
                                joined_context.fetch_members.clear();
                                joined_context.joined_fetch_prefixes.clear();
                                entity.[:m:] = map_row_to_entity<Target>(rs, &joined_context);
                            } else {
                                entity.[:m:] = eager_load_many_to_one<Target>(rs, col_idx, context);
                            }
                        } else if constexpr (many_to_one.fetch == novaboot::annotations::FetchType::Lazy) {
                            entity.[:m:] = entity_with_id<Target>(rs, col_idx);
                        } else {
                            entity.[:m:] = eager_load_many_to_one<Target>(rs, col_idx, context);
                        }
                    } else if constexpr (many_to_one.fetch == novaboot::annotations::FetchType::Lazy) {
                        if (should_fetch_member(context, member_name)) {
                            if (auto prefix = joined_fetch_prefix(context, member_name)) {
                                auto joined_context = context ? *context : EntityLoadContext{};
                                joined_context.column_prefix = *prefix;
                                joined_context.fetch_members.clear();
                                joined_context.joined_fetch_prefixes.clear();
                                entity.[:m:] = map_row_to_entity<FT>(rs, &joined_context);
                            } else {
                                entity.[:m:] = eager_load_many_to_one<FT>(rs, col_idx, context);
                            }
                        } else {
                            entity.[:m:] = entity_with_id<FT>(rs, col_idx);
                        }
                    } else {
                        entity.[:m:] = eager_load_many_to_one<FT>(rs, col_idx, context);
                    }
                } else if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Json>(m)) {
                    if constexpr (std::is_same_v<FT, std::string>) {
                        entity.[:m:] = rs->get_string(col_idx);
                    } else {
                        entity.[:m:] = novaboot::json::deserialize<FT>(rs->get_string(col_idx));
                    }
                } else if constexpr (std::is_same_v<FT, std::chrono::system_clock::time_point> &&
                                     novaboot::di::detail::has_annotation<novaboot::annotations::Temporal>(m)) {
                    constexpr auto temporal = novaboot::di::detail::get_annotation<novaboot::annotations::Temporal>(m);
                    entity.[:m:] = temporal_from_string(rs->get_string(col_idx), temporal.value);
                } else if constexpr (std::is_enum_v<FT>) {
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
                } else if constexpr (std::is_same_v<FT, int> || std::is_same_v<FT, std::int64_t>) {
                    entity.[:m:] = static_cast<FT>(rs->get_int(col_idx));
                } else if constexpr (std::is_same_v<FT, double> || std::is_same_v<FT, float>) {
                    entity.[:m:] = static_cast<FT>(rs->get_double(col_idx));
                } else if constexpr (std::is_same_v<FT, std::string>) {
                    entity.[:m:] = rs->get_string(col_idx);
                } else if constexpr (std::is_same_v<FT, bool>) {
                    entity.[:m:] = rs->get_bool(col_idx);
                } else if constexpr (std::is_same_v<FT, std::vector<std::uint8_t>>) {
                    entity.[:m:] = rs->get_blob(col_idx);
                } else if constexpr (std::is_same_v<FT, Uuid>) {
                    entity.[:m:] = rs->get_uuid(col_idx);
                } else if constexpr (std::is_same_v<FT, std::chrono::system_clock::time_point>) {
                    entity.[:m:] = rs->get_time(col_idx);
                }
            }
        }
    }

    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m) &&
                      novaboot::di::detail::has_annotation<novaboot::annotations::OneToMany>(m)) {
            using FT = std::remove_cvref_t<decltype(entity.[:m:])>;
            constexpr auto one_to_many = novaboot::di::detail::get_annotation<novaboot::annotations::OneToMany>(m);
            static_assert(is_collection_relation_v<FT>,
                          "@OneToMany fields must be std::vector<T> or LazyCollection<T>");
            static_assert(one_to_many.mapped_by[0] != '\0', "@OneToMany requires mapped_by");
            constexpr auto member_name = std::meta::identifier_of(m);

            using Child = typename vector_value_type<FT>::type;
            if constexpr (is_lazy_collection<FT>::value) {
                if (one_to_many.fetch == novaboot::annotations::FetchType::Eager ||
                    should_fetch_member(context, member_name)) {
                    entity.[:m:] = FT::loaded(eager_load_one_to_many<T, Child>(
                        entity, one_to_many.mapped_by, context));
                } else {
                    entity.[:m:] = lazy_load_one_to_many<T, Child>(
                        entity, one_to_many.mapped_by, context);
                }
            } else {
                if (one_to_many.fetch == novaboot::annotations::FetchType::Eager ||
                    should_fetch_member(context, member_name)) {
                    entity.[:m:] = eager_load_one_to_many<T, Child>(
                        entity, one_to_many.mapped_by, context);
                }
            }
        } else if constexpr (std::meta::is_nonstatic_data_member(m) &&
                             novaboot::di::detail::has_annotation<novaboot::annotations::ManyToMany>(m)) {
            using FT = std::remove_cvref_t<decltype(entity.[:m:])>;
            constexpr auto many_to_many = novaboot::di::detail::get_annotation<novaboot::annotations::ManyToMany>(m);
            static_assert(is_collection_relation_v<FT>,
                          "@ManyToMany fields must be std::vector<T> or LazyCollection<T>");
            static_assert(novaboot::di::detail::has_annotation<novaboot::annotations::JoinTable>(m),
                          "@ManyToMany requires @JoinTable");
            constexpr auto join_table = novaboot::di::detail::get_annotation<novaboot::annotations::JoinTable>(m);
            static_assert(join_table.name[0] != '\0' &&
                          join_table.join_column[0] != '\0' &&
                          join_table.inverse_join_column[0] != '\0',
                          "@JoinTable requires name, join_column, and inverse_join_column");
            constexpr auto member_name = std::meta::identifier_of(m);

            using Child = typename vector_value_type<FT>::type;
            if constexpr (is_lazy_collection<FT>::value) {
                if (many_to_many.fetch == novaboot::annotations::FetchType::Eager ||
                    should_fetch_member(context, member_name)) {
                    entity.[:m:] = FT::loaded(eager_load_many_to_many<T, Child>(
                        entity, join_table.name, join_table.join_column,
                        join_table.inverse_join_column, context));
                } else {
                    entity.[:m:] = lazy_load_many_to_many<T, Child>(
                        entity, join_table.name, join_table.join_column,
                        join_table.inverse_join_column, context);
                }
            } else {
                if (many_to_many.fetch == novaboot::annotations::FetchType::Eager ||
                    should_fetch_member(context, member_name)) {
                    entity.[:m:] = eager_load_many_to_many<T, Child>(
                        entity, join_table.name, join_table.join_column,
                        join_table.inverse_join_column, context);
                }
            }
        }
    }

    invoke_lifecycle<novaboot::annotations::PostLoad>(entity);
    return entity;
}

} // namespace novaboot::db::detail
