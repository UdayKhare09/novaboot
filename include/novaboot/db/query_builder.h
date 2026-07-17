#pragma once
#include "novaboot/db/db_client.h"
#include "novaboot/db/orm_reflection.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <stdexcept>
#include <array>
#include <cstdint>
#include <functional>
#include <iterator>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace novaboot::db {

enum class Op { Equal, LessThan, GreaterThan, Like, NotEqual, LessThanOrEqual, GreaterThanOrEqual };

struct Sort {
    std::string column;
    bool ascending = true;
};

struct Pageable {
    int page = 0;
    int size = 20;
    std::vector<Sort> sort;
};

template<typename Entity>
struct Page {
    std::vector<Entity> content;
    int page = 0;
    int size = 0;
    std::int64_t total_elements = 0;

    std::int64_t total_pages() const {
        return size == 0 ? 0 : (total_elements + size - 1) / size;
    }

    bool has_next() const {
        return static_cast<std::int64_t>(page + 1) < total_pages();
    }
};

template<typename Entity>
class QueryBuilder {
private:
    std::shared_ptr<DataSource> datasource_;
    std::shared_ptr<Connection> connection_;
    std::string table_name_;
    std::string sql_where_;
    std::vector<Parameter> params_;
    
    std::string sql_order_;
    int limit_val_ = -1;
    int offset_val_ = -1;
    std::unordered_set<std::string> fetch_to_one_members_;
    std::unordered_set<std::string> fetch_collection_members_;

    template<typename T>
    static Parameter make_parameter(const T& value) {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::is_enum_v<Value>) {
            return Parameter(static_cast<std::int64_t>(value));
        } else if constexpr (detail::is_lazy_relation<Value>::value) {
            return detail::entity_id_parameter(value);
        } else if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Entity>(^^Value)) {
            return detail::entity_id_parameter(value);
        } else if constexpr (std::is_integral_v<Value> && !std::is_same_v<Value, bool>) {
            return Parameter(static_cast<std::int64_t>(value));
        } else if constexpr (std::is_floating_point_v<Value>) {
            return Parameter(static_cast<double>(value));
        } else if constexpr (std::is_convertible_v<T, std::string_view> &&
                             !std::is_same_v<Value, std::string>) {
            return Parameter(std::string(std::string_view(value)));
        } else {
            return Parameter(value);
        }
    }

    template<auto FieldPtr, typename T>
    static Parameter make_field_parameter(const T& value) {
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().*FieldPtr)>;
        if constexpr (std::is_enum_v<Field>) {
            constexpr auto member = detail::get_member_info_for_field<Entity, FieldPtr>();
            if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Enumerated>(member)) {
                constexpr auto enumerated =
                    novaboot::di::detail::get_annotation<novaboot::annotations::Enumerated>(member);
                if constexpr (enumerated.value == novaboot::annotations::EnumType::String) {
                    return Parameter(detail::enum_name(static_cast<Field>(value)));
                }
            }
        }
        return make_parameter(value);
    }

    template<typename Target, auto FieldPtr, typename T>
    static Parameter make_target_field_parameter(const T& value) {
        using Field = std::remove_cvref_t<decltype(std::declval<Target&>().*FieldPtr)>;
        if constexpr (std::is_enum_v<Field>) {
            constexpr auto member = detail::get_member_info_for_field<Target, FieldPtr>();
            if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Enumerated>(member)) {
                constexpr auto enumerated =
                    novaboot::di::detail::get_annotation<novaboot::annotations::Enumerated>(member);
                if constexpr (enumerated.value == novaboot::annotations::EnumType::String) {
                    return Parameter(detail::enum_name(static_cast<Field>(value)));
                }
            }
        }
        return make_parameter(value);
    }

    void append_condition(std::string condition, std::vector<Parameter> params,
                          std::string_view connector = "AND") {
        if (sql_where_.empty()) {
            sql_where_ = " WHERE " + std::move(condition);
        } else {
            sql_where_ += " " + std::string(connector) + " " + condition;
        }
        params_.insert(params_.end(),
                       std::make_move_iterator(params.begin()),
                       std::make_move_iterator(params.end()));
    }

    template<auto FieldPtr, std::ranges::input_range Range>
    void append_in(const Range& values, std::string_view connector) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        std::vector<Parameter> params;
        std::string placeholders;

        for (const auto& value : values) {
            if (!placeholders.empty()) placeholders += ", ";
            placeholders += "?";
            params.push_back(make_field_parameter<FieldPtr>(value));
        }

        // SQL has no portable IN ().  An empty membership set is always false.
        if (params.empty()) {
            append_condition("1 = 0", {}, connector);
            return;
        }
        append_condition(std::string(col.name) + " IN (" + placeholders + ")",
                         std::move(params), connector);
    }

    template<typename Configure>
    QueryBuilder& append_group(Configure&& configure, std::string_view connector) {
        QueryBuilder group(datasource_, table_name_);
        std::invoke(std::forward<Configure>(configure), group);
        if (group.sql_where_.empty()) {
            throw std::invalid_argument("QueryBuilder group must contain a predicate");
        }

        append_condition("(" + group.sql_where_.substr(7) + ")",
                         std::move(group.params_), connector);
        return *this;
    }
    
    std::string get_op_symbol(Op op) {
        switch (op) {
            case Op::Equal:              return " = ";
            case Op::LessThan:           return " < ";
            case Op::GreaterThan:        return " > ";
            case Op::Like:               return " LIKE ";
            case Op::NotEqual:           return " != ";
            case Op::LessThanOrEqual:    return " <= ";
            case Op::GreaterThanOrEqual: return " >= ";
        }
        return " = ";
    }

    void append_order_by_column(std::string_view column, bool ascending) {
        if (!detail::is_persisted_column<Entity>(column)) {
            throw std::invalid_argument("QueryBuilder sort column is not a persisted entity field: " +
                                        std::string(column));
        }
        if (sql_order_.empty()) {
            sql_order_ = " ORDER BY ";
        } else {
            sql_order_ += ", ";
        }
        sql_order_ += std::string(column) + (ascending ? " ASC" : " DESC");
    }

    std::vector<Parameter> parent_ids_for(const std::vector<Entity>& results,
                                          detail::EntityLoadContext& load_context) const {
        std::vector<Parameter> ids;
        std::unordered_set<std::string> seen;
        const auto parent_table = detail::entity_table_name<Entity>();

        for (const auto& entity : results) {
            auto id = detail::entity_id_parameter(entity);
            const auto key = format_parameter(id);
            load_context.loading.insert(parent_table + ":" + key);
            if (seen.insert(key).second) {
                ids.push_back(std::move(id));
            }
        }

        return ids;
    }

    static std::string placeholders_for(std::size_t count) {
        std::string placeholders;
        for (std::size_t i = 0; i < count; ++i) {
            if (!placeholders.empty()) placeholders += ", ";
            placeholders += "?";
        }
        return placeholders;
    }

    static std::string join_fetch_prefix(std::string_view member_name) {
        return "__novaboot_" + std::string(member_name) + "_";
    }

    static std::string join_fetch_alias(std::string_view member_name) {
        return "__nb_" + std::string(member_name);
    }

    void configure_joined_to_one_fetches(detail::EntityLoadContext& load_context) const {
        static constexpr auto members = detail::get_members<Entity>();
        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(member)) {
                constexpr auto member_name = std::meta::identifier_of(member);
                if (!fetch_to_one_members_.contains(std::string(member_name))) continue;
                load_context.joined_fetch_prefixes.emplace(
                    std::string(member_name), join_fetch_prefix(member_name));
            }
        }
    }

    void append_joined_to_one_sql(std::string& select_columns,
                                  std::string& joins) const {
        static constexpr auto members = detail::get_members<Entity>();
        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(member)) {
                constexpr auto member_name = std::meta::identifier_of(member);
                if (!fetch_to_one_members_.contains(std::string(member_name))) continue;

                using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:member:])>;
                using Target = typename detail::relation_value_type<Field>::type;
                constexpr auto join_column = detail::get_member_column_name<member>();
                const auto alias = join_fetch_alias(member_name);
                const auto prefix = join_fetch_prefix(member_name);
                const auto target_table = detail::entity_table_name<Target>();
                const auto target_pk = detail::entity_primary_key_column<Target>();

                select_columns += ", " +
                    detail::get_select_column_list_with_aliases<Target>(alias, prefix);
                joins += " LEFT JOIN " + target_table + " " + alias +
                         " ON nb_root." + std::string(join_column.name) +
                         " = " + alias + "." + target_pk;
            }
        }
    }

    std::string build_list_sql(const std::shared_ptr<SqlDialect>& dialect) const {
        std::string base_sql = "SELECT " + detail::get_select_column_list<Entity>() +
                               " FROM " + table_name_ + sql_where_ + sql_order_;
        base_sql += dialect->compile_pagination(limit_val_, offset_val_);

        if (fetch_to_one_members_.empty()) return base_sql;

        std::string select_columns = detail::get_select_column_list<Entity>("nb_root");
        std::string joins;
        append_joined_to_one_sql(select_columns, joins);
        return "SELECT " + select_columns + " FROM (" + base_sql + ") nb_root" + joins;
    }

    template<typename Child>
    std::unordered_map<std::string, std::vector<Child>>
    batch_load_one_to_many(const std::vector<Entity>& results,
                           std::string_view mapped_by,
                           detail::EntityLoadContext& load_context) const {
        std::unordered_map<std::string, std::vector<Child>> grouped;
        if (results.empty()) return grouped;

        auto parent_ids = parent_ids_for(results, load_context);
        if (parent_ids.empty()) return grouped;

        const auto child_table = detail::entity_table_name<Child>();
        const auto join_column = detail::child_join_column_for_mapped_by<Child>(mapped_by);
        const auto parent_alias = "__novaboot_parent_id";
        const auto sql = "SELECT " + detail::get_select_column_list<Child>() + ", " +
                         join_column + " AS " + parent_alias +
                         " FROM " + child_table +
                         " WHERE " + join_column + " IN (" +
                         placeholders_for(parent_ids.size()) + ")";
        auto dialect = datasource_->dialect();
        auto result = load_context.connection->query(dialect->convert_placeholders(sql), parent_ids);

        while (result->next()) {
            const int parent_col = detail::find_column_index(result.get(), parent_alias);
            if (parent_col < 0 || result->is_null(parent_col)) continue;
            auto parent_identity = detail::entity_with_id<Entity>(result.get(), parent_col);
            auto child = detail::map_row_to_entity<Child>(result.get(), &load_context);
            grouped[format_parameter(detail::entity_id_parameter(parent_identity))]
                .push_back(std::move(child));
        }

        return grouped;
    }

    template<typename Child>
    std::unordered_map<std::string, std::vector<Child>>
    batch_load_many_to_many(const std::vector<Entity>& results,
                            std::string_view join_table,
                            std::string_view join_column,
                            std::string_view inverse_join_column,
                            detail::EntityLoadContext& load_context) const {
        std::unordered_map<std::string, std::vector<Child>> grouped;
        if (results.empty()) return grouped;

        auto parent_ids = parent_ids_for(results, load_context);
        if (parent_ids.empty()) return grouped;

        const auto child_table = detail::entity_table_name<Child>();
        const auto child_pk = detail::entity_primary_key_column<Child>();
        const auto parent_alias = "__novaboot_parent_id";
        const auto sql = "SELECT jt." + std::string(join_column) + " AS " + parent_alias +
                         ", " + detail::get_select_column_list<Child>("c") +
                         " FROM " + child_table + " c JOIN " + std::string(join_table) +
                         " jt ON c." + child_pk + " = jt." + std::string(inverse_join_column) +
                         " WHERE jt." + std::string(join_column) + " IN (" +
                         placeholders_for(parent_ids.size()) + ")";
        auto dialect = datasource_->dialect();
        auto result = load_context.connection->query(dialect->convert_placeholders(sql), parent_ids);

        while (result->next()) {
            const int parent_col = detail::find_column_index(result.get(), parent_alias);
            if (parent_col < 0 || result->is_null(parent_col)) continue;
            auto parent_identity = detail::entity_with_id<Entity>(result.get(), parent_col);
            auto child = detail::map_row_to_entity<Child>(result.get(), &load_context);
            grouped[format_parameter(detail::entity_id_parameter(parent_identity))]
                .push_back(std::move(child));
        }

        return grouped;
    }

    void hydrate_requested_collections(std::vector<Entity>& results,
                                       const std::shared_ptr<Connection>& conn) const {
        if (fetch_collection_members_.empty()) return;

        detail::EntityLoadContext load_context;
        load_context.datasource = datasource_.get();
        load_context.connection = conn;
        load_context.retain_connection_for_lazy = connection_ != nullptr;

        static constexpr auto members = detail::get_members<Entity>();
        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::OneToMany>(member)) {
                constexpr auto member_name = std::meta::identifier_of(member);
                if (!fetch_collection_members_.contains(std::string(member_name))) continue;

                using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:member:])>;
                using Child = typename detail::vector_value_type<Field>::type;
                constexpr auto one_to_many =
                    novaboot::di::detail::get_annotation<novaboot::annotations::OneToMany>(member);
                auto grouped = batch_load_one_to_many<Child>(
                    results, one_to_many.mapped_by, load_context);
                for (auto& entity : results) {
                    auto values = std::move(grouped[format_parameter(detail::entity_id_parameter(entity))]);
                    if constexpr (detail::is_lazy_collection<Field>::value) {
                        entity.[:member:] = Field::loaded(std::move(values));
                    } else {
                        entity.[:member:] = std::move(values);
                    }
                }
            } else if constexpr (std::meta::is_nonstatic_data_member(member) &&
                                 novaboot::di::detail::has_annotation<novaboot::annotations::ManyToMany>(member)) {
                constexpr auto member_name = std::meta::identifier_of(member);
                if (!fetch_collection_members_.contains(std::string(member_name))) continue;

                using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:member:])>;
                using Child = typename detail::vector_value_type<Field>::type;
                constexpr auto join_table =
                    novaboot::di::detail::get_annotation<novaboot::annotations::JoinTable>(member);
                auto grouped = batch_load_many_to_many<Child>(
                    results, join_table.name, join_table.join_column,
                    join_table.inverse_join_column, load_context);
                for (auto& entity : results) {
                    auto values = std::move(grouped[format_parameter(detail::entity_id_parameter(entity))]);
                    if constexpr (detail::is_lazy_collection<Field>::value) {
                        entity.[:member:] = Field::loaded(std::move(values));
                    } else {
                        entity.[:member:] = std::move(values);
                    }
                }
            }
        }
    }

public:
    QueryBuilder(std::shared_ptr<DataSource> ds, std::string table_name,
                 std::shared_ptr<Connection> connection = nullptr)
        : datasource_(std::move(ds)), connection_(std::move(connection)),
          table_name_(std::move(table_name)) {}

    template<auto FieldPtr, typename Val>
    QueryBuilder& where(Op op, const Val& val) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        sql_where_.clear();
        params_.clear();
        append_condition(std::string(col.name) + get_op_symbol(op) + "?",
                         {make_field_parameter<FieldPtr>(val)});
        return *this;
    }

    template<auto RelationPtr, auto TargetFieldPtr, typename Val>
    QueryBuilder& where_related(Op op, const Val& val) {
        sql_where_.clear();
        params_.clear();
        return and_related<RelationPtr, TargetFieldPtr>(op, val);
    }

    template<auto FieldPtr, typename Val>
    QueryBuilder& and_(Op op, const Val& val) {
        if (sql_where_.empty()) {
            return where<FieldPtr>(op, val);
        }
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + get_op_symbol(op) + "?",
                         {make_field_parameter<FieldPtr>(val)});
        return *this;
    }

    template<auto RelationPtr, auto TargetFieldPtr, typename Val>
    QueryBuilder& and_related(Op op, const Val& val) {
        constexpr auto relation_member = detail::get_member_info_for_field<Entity, RelationPtr>();
        static_assert(relation_member != std::meta::info{},
                      "QueryBuilder relation field must belong to the entity");
        static_assert(novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(relation_member),
                      "QueryBuilder related predicates currently require a @ManyToOne field");

        using RelationField = std::remove_cvref_t<decltype(std::declval<Entity&>().*RelationPtr)>;
        using Target = typename detail::relation_value_type<RelationField>::type;
        constexpr auto relation_col = detail::get_member_column_name<relation_member>();
        constexpr auto target_col = detail::get_column_name<Target, TargetFieldPtr>();

        const auto target_table = detail::entity_table_name<Target>();
        const auto target_pk = detail::entity_primary_key_column<Target>();
        const auto condition = std::string(relation_col.name) + " IN (SELECT " +
                               target_pk + " FROM " + target_table + " WHERE " +
                               std::string(target_col.name) + get_op_symbol(op) + "?)";
        append_condition(condition, {make_target_field_parameter<Target, TargetFieldPtr>(val)});
        return *this;
    }

    template<auto FieldPtr, typename Val>
    QueryBuilder& or_(Op op, const Val& val) {
        if (sql_where_.empty()) {
            return where<FieldPtr>(op, val);
        }
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + get_op_symbol(op) + "?",
                         {make_field_parameter<FieldPtr>(val)}, "OR");
        return *this;
    }

    template<auto RelationPtr, auto TargetFieldPtr, typename Val>
    QueryBuilder& or_related(Op op, const Val& val) {
        if (sql_where_.empty()) {
            return where_related<RelationPtr, TargetFieldPtr>(op, val);
        }

        constexpr auto relation_member = detail::get_member_info_for_field<Entity, RelationPtr>();
        static_assert(relation_member != std::meta::info{},
                      "QueryBuilder relation field must belong to the entity");
        static_assert(novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(relation_member),
                      "QueryBuilder related predicates currently require a @ManyToOne field");

        using RelationField = std::remove_cvref_t<decltype(std::declval<Entity&>().*RelationPtr)>;
        using Target = typename detail::relation_value_type<RelationField>::type;
        constexpr auto relation_col = detail::get_member_column_name<relation_member>();
        constexpr auto target_col = detail::get_column_name<Target, TargetFieldPtr>();

        const auto target_table = detail::entity_table_name<Target>();
        const auto target_pk = detail::entity_primary_key_column<Target>();
        const auto condition = std::string(relation_col.name) + " IN (SELECT " +
                               target_pk + " FROM " + target_table + " WHERE " +
                               std::string(target_col.name) + get_op_symbol(op) + "?)";
        append_condition(condition,
                         {make_target_field_parameter<Target, TargetFieldPtr>(val)},
                         "OR");
        return *this;
    }

    template<auto FieldPtr>
    QueryBuilder& where_is_null() {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        sql_where_.clear();
        params_.clear();
        append_condition(std::string(col.name) + " IS NULL", {});
        return *this;
    }

    template<auto FieldPtr>
    QueryBuilder& where_is_not_null() {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        sql_where_.clear();
        params_.clear();
        append_condition(std::string(col.name) + " IS NOT NULL", {});
        return *this;
    }

    template<auto FieldPtr>
    QueryBuilder& and_is_null() {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + " IS NULL", {});
        return *this;
    }

    template<auto FieldPtr>
    QueryBuilder& or_is_null() {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + " IS NULL", {}, "OR");
        return *this;
    }

    template<auto FieldPtr, std::ranges::input_range Range>
    QueryBuilder& where_in(const Range& values) {
        sql_where_.clear();
        params_.clear();
        append_in<FieldPtr>(values, "AND");
        return *this;
    }

    template<auto FieldPtr, std::ranges::input_range Range>
    QueryBuilder& and_in(const Range& values) {
        append_in<FieldPtr>(values, "AND");
        return *this;
    }

    template<auto FieldPtr, typename Lower, typename Upper>
    QueryBuilder& where_between(const Lower& lower, const Upper& upper) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        sql_where_.clear();
        params_.clear();
        append_condition(std::string(col.name) + " BETWEEN ? AND ?",
                         {make_field_parameter<FieldPtr>(lower),
                          make_field_parameter<FieldPtr>(upper)});
        return *this;
    }

    template<auto FieldPtr, typename Lower, typename Upper>
    QueryBuilder& and_between(const Lower& lower, const Upper& upper) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + " BETWEEN ? AND ?",
                         {make_field_parameter<FieldPtr>(lower),
                          make_field_parameter<FieldPtr>(upper)});
        return *this;
    }

    template<typename Configure>
    QueryBuilder& where_group(Configure&& configure) {
        sql_where_.clear();
        params_.clear();
        return append_group(std::forward<Configure>(configure), "AND");
    }

    template<typename Configure>
    QueryBuilder& and_group(Configure&& configure) {
        return append_group(std::forward<Configure>(configure), "AND");
    }

    template<typename Configure>
    QueryBuilder& or_group(Configure&& configure) {
        return append_group(std::forward<Configure>(configure), "OR");
    }

    template<auto FieldPtr>
    QueryBuilder& order_by(bool ascending = true) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_order_by_column(col.name, ascending);
        return *this;
    }

    template<auto RelationPtr, auto TargetFieldPtr>
    QueryBuilder& order_by_related(bool ascending = true) {
        constexpr auto relation_member = detail::get_member_info_for_field<Entity, RelationPtr>();
        static_assert(relation_member != std::meta::info{},
                      "QueryBuilder relation field must belong to the entity");
        static_assert(novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(relation_member),
                      "QueryBuilder related sorting currently requires a @ManyToOne field");

        using RelationField = std::remove_cvref_t<decltype(std::declval<Entity&>().*RelationPtr)>;
        using Target = typename detail::relation_value_type<RelationField>::type;
        constexpr auto relation_col = detail::get_member_column_name<relation_member>();
        constexpr auto target_col = detail::get_column_name<Target, TargetFieldPtr>();

        const auto target_table = detail::entity_table_name<Target>();
        const auto target_pk = detail::entity_primary_key_column<Target>();
        const auto order_expr = "(SELECT " + std::string(target_col.name) +
                                " FROM " + target_table +
                                " WHERE " + target_pk + " = " +
                                std::string(relation_col.name) + ")";
        if (sql_order_.empty()) {
            sql_order_ = " ORDER BY ";
        } else {
            sql_order_ += ", ";
        }
        sql_order_ += order_expr + (ascending ? " ASC" : " DESC");
        return *this;
    }

    template<auto FieldPtr>
    QueryBuilder& fetch() {
        constexpr auto member = detail::get_member_info_for_field<Entity, FieldPtr>();
        static_assert(member != std::meta::info{}, "QueryBuilder::fetch field must belong to the entity");
        static_assert(
            novaboot::di::detail::has_annotation<novaboot::annotations::ManyToOne>(member) ||
            novaboot::di::detail::has_annotation<novaboot::annotations::OneToMany>(member) ||
            novaboot::di::detail::has_annotation<novaboot::annotations::ManyToMany>(member),
            "QueryBuilder::fetch requires a relationship field");
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().*FieldPtr)>;
        if constexpr (detail::is_collection_relation_v<Field> ||
                      novaboot::di::detail::has_annotation<novaboot::annotations::OneToMany>(member) ||
                      novaboot::di::detail::has_annotation<novaboot::annotations::ManyToMany>(member)) {
            fetch_collection_members_.insert(std::string(std::meta::identifier_of(member)));
        } else {
            fetch_to_one_members_.insert(std::string(std::meta::identifier_of(member)));
        }
        return *this;
    }

    QueryBuilder& limit(int count) {
        limit_val_ = count;
        return *this;
    }

    QueryBuilder& offset(int count) {
        offset_val_ = count;
        return *this;
    }

    std::vector<Entity> list() {
        auto dialect = datasource_->dialect();
        std::string sql = build_list_sql(dialect);
        std::string final_sql = dialect->convert_placeholders(sql);
        
        auto conn = connection_ ? connection_ : datasource_->get_connection();
        auto rs = conn->query(final_sql, params_);
        
        std::vector<Entity> results;
        detail::EntityLoadContext load_context;
        load_context.datasource = datasource_.get();
        load_context.connection = conn;
        load_context.retain_connection_for_lazy = connection_ != nullptr;
        load_context.fetch_members = fetch_to_one_members_;
        configure_joined_to_one_fetches(load_context);
        while (rs->next()) {
            results.push_back(detail::map_row_to_entity<Entity>(rs.get(), &load_context));
        }
        rs.reset();
        hydrate_requested_collections(results, conn);
        return results;
    }

    std::optional<Entity> single() {
        limit(1);
        auto results = list();
        if (!results.empty()) return results[0];
        return std::nullopt;
    }

    template<typename Projection>
    std::vector<Projection> project() {
        auto dialect = datasource_->dialect();
        std::string sql = "SELECT " + detail::get_select_column_list<Projection>() +
                          " FROM " + table_name_ + sql_where_ + sql_order_;
        sql += dialect->compile_pagination(limit_val_, offset_val_);

        auto conn = connection_ ? connection_ : datasource_->get_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), params_);

        std::vector<Projection> results;
        while (rs->next()) {
            results.push_back(detail::map_row_to_entity<Projection>(rs.get()));
        }
        return results;
    }

    template<typename Projection>
    std::optional<Projection> project_single() {
        limit(1);
        auto results = project<Projection>();
        if (!results.empty()) return results[0];
        return std::nullopt;
    }

    std::int64_t count() const {
        auto dialect = datasource_->dialect();
        std::string sql = "SELECT COUNT(1) FROM " + table_name_ + sql_where_;
        auto conn = connection_ ? connection_ : datasource_->get_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), params_);
        return rs->next() ? rs->get_int(0) : 0;
    }

    bool exists() const {
        auto dialect = datasource_->dialect();
        std::string sql = "SELECT 1 FROM " + table_name_ + sql_where_ +
                          dialect->compile_pagination(1, -1);
        auto conn = connection_ ? connection_ : datasource_->get_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), params_);
        return rs->next();
    }

    Page<Entity> page(const Pageable& pageable) const {
        if (pageable.page < 0 || pageable.size <= 0) {
            throw std::invalid_argument("Pageable requires a non-negative page and a positive size");
        }

        QueryBuilder paged = *this;
        for (const auto& sort : pageable.sort) {
            paged.append_order_by_column(sort.column, sort.ascending);
        }
        paged.limit(pageable.size)
             .offset(pageable.page * pageable.size);

        const auto total = count();

        return Page<Entity>{
            .content = paged.list(),
            .page = pageable.page,
            .size = pageable.size,
            .total_elements = total,
        };
    }
};

} // namespace novaboot::db
