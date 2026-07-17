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

    template<typename T>
    static Parameter make_parameter(const T& value) {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::is_enum_v<Value>) {
            return Parameter(static_cast<std::int64_t>(value));
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
            params.push_back(make_parameter(value));
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
                         {make_parameter(val)});
        return *this;
    }

    template<auto FieldPtr, typename Val>
    QueryBuilder& and_(Op op, const Val& val) {
        if (sql_where_.empty()) {
            return where<FieldPtr>(op, val);
        }
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + get_op_symbol(op) + "?",
                         {make_parameter(val)});
        return *this;
    }

    template<auto FieldPtr, typename Val>
    QueryBuilder& or_(Op op, const Val& val) {
        if (sql_where_.empty()) {
            return where<FieldPtr>(op, val);
        }
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + get_op_symbol(op) + "?",
                         {make_parameter(val)}, "OR");
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
                         {make_parameter(lower), make_parameter(upper)});
        return *this;
    }

    template<auto FieldPtr, typename Lower, typename Upper>
    QueryBuilder& and_between(const Lower& lower, const Upper& upper) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        append_condition(std::string(col.name) + " BETWEEN ? AND ?",
                         {make_parameter(lower), make_parameter(upper)});
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
        std::string sql = "SELECT " + detail::get_select_column_list<Entity>() +
                          " FROM " + table_name_ + sql_where_ + sql_order_;
        sql += dialect->compile_pagination(limit_val_, offset_val_);
        std::string final_sql = dialect->convert_placeholders(sql);
        
        auto conn = connection_ ? connection_ : datasource_->get_connection();
        auto rs = conn->query(final_sql, params_);
        
        std::vector<Entity> results;
        detail::EntityLoadContext load_context;
        load_context.datasource = datasource_.get();
        load_context.connection = conn;
        while (rs->next()) {
            results.push_back(detail::map_row_to_entity<Entity>(rs.get(), &load_context));
        }
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
