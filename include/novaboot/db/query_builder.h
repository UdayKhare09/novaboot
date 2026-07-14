#pragma once
#include "novaboot/db/db_client.h"
#include "novaboot/db/orm_reflection.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <stdexcept>
#include <array>

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
class QueryBuilder {
private:
    std::shared_ptr<DataSource> datasource_;
    std::string table_name_;
    std::string sql_where_;
    std::vector<Parameter> params_;
    
    std::string sql_order_;
    std::string sql_limit_;
    
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

public:
    QueryBuilder(std::shared_ptr<DataSource> ds, std::string table_name)
        : datasource_(ds), table_name_(std::move(table_name)) {}

    template<auto FieldPtr, typename Val>
    QueryBuilder& where(Op op, const Val& val) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        sql_where_ = " WHERE " + std::string(col.name) + get_op_symbol(op) + "?";
        params_.push_back(Parameter(val));
        return *this;
    }

    template<auto FieldPtr, typename Val>
    QueryBuilder& and_(Op op, const Val& val) {
        if (sql_where_.empty()) {
            return where<FieldPtr>(op, val);
        }
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        sql_where_ += " AND " + std::string(col.name) + get_op_symbol(op) + "?";
        params_.push_back(Parameter(val));
        return *this;
    }

    template<auto FieldPtr, typename Val>
    QueryBuilder& or_(Op op, const Val& val) {
        if (sql_where_.empty()) {
            return where<FieldPtr>(op, val);
        }
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        sql_where_ += " OR " + std::string(col.name) + get_op_symbol(op) + "?";
        params_.push_back(Parameter(val));
        return *this;
    }

    template<auto FieldPtr>
    QueryBuilder& order_by(bool ascending = true) {
        constexpr auto col = detail::get_column_name<Entity, FieldPtr>();
        if (sql_order_.empty()) {
            sql_order_ = " ORDER BY " + std::string(col.name) + (ascending ? " ASC" : " DESC");
        } else {
            sql_order_ += ", " + std::string(col.name) + (ascending ? " ASC" : " DESC");
        }
        return *this;
    }

    QueryBuilder& limit(int count) {
        if (count >= 0) {
            sql_limit_ = " LIMIT " + std::to_string(count);
        }
        return *this;
    }

    QueryBuilder& offset(int count) {
        if (count >= 0 && !sql_limit_.empty()) {
            sql_limit_ += " OFFSET " + std::to_string(count);
        }
        return *this;
    }

    std::vector<Entity> list() {
        std::string sql = "SELECT * FROM " + table_name_ + sql_where_ + sql_order_ + sql_limit_;
        auto conn = datasource_->get_connection();
        auto rs = conn->query(sql, params_);
        
        std::vector<Entity> results;
        while (rs->next()) {
            results.push_back(detail::map_row_to_entity<Entity>(rs.get()));
        }
        return results;
    }

    std::optional<Entity> single() {
        limit(1);
        auto results = list();
        if (!results.empty()) return results[0];
        return std::nullopt;
    }
};

} // namespace novaboot::db
