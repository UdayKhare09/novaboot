#pragma once
#include "novaboot/db/db_client.h"
#include "novaboot/db/query_builder.h"
#include "novaboot/db/orm_reflection.h"
#include "novaboot/di/container.h"
#include <memory>
#include <vector>
#include <optional>
#include <string>

namespace novaboot::db {

template<typename Entity, typename ID>
class CrudRepository {
protected:
    std::shared_ptr<DataSource> datasource_;
    std::string table_name_;
    std::string pk_col_name_;

    void extract_metadata() {
        constexpr auto ent = detail::get_table_metadata<Entity>();
        if constexpr (ent.name[0] != '\0') {
            table_name_ = ent.name;
        } else {
            constexpr auto raw_name = std::meta::identifier_of(^^Entity);
            table_name_ = detail::to_snake_case(raw_name) + "s";
        }
        
        // Find primary key column from @Id annotation
        static constexpr auto members = detail::get_members<Entity>();
        pk_col_name_ = "id"; // default fallback
        
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Id>(m)) {
                    pk_col_name_ = std::string(detail::get_member_column_name<m>().name);
                }
            }
        }
    }

public:
    explicit CrudRepository(std::shared_ptr<DataSource> ds) : datasource_(ds) {
        extract_metadata();
    }

    virtual ~CrudRepository() = default;

    /// Get clean query builder session
    QueryBuilder<Entity> query() {
        return QueryBuilder<Entity>(datasource_, table_name_);
    }

    /// Primary Key Lookup
    std::optional<Entity> find_by_id(const ID& id) {
        std::string sql = "SELECT * FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto conn = datasource_->get_connection();
        auto rs = conn->query(sql, { Parameter(id) });
        if (rs->next()) {
            return detail::map_row_to_entity<Entity>(rs.get());
        }
        return std::nullopt;
    }

    /// Exists verification
    bool exists_by_id(const ID& id) {
        std::string sql = "SELECT COUNT(1) FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto conn = datasource_->get_connection();
        auto rs = conn->query(sql, { Parameter(id) });
        if (rs->next()) {
            return rs->get_int(0) > 0;
        }
        return false;
    }

    /// Retrieve all records
    std::vector<Entity> find_all() {
        return query().list();
    }

    /// Delete by id
    void delete_by_id(const ID& id) {
        std::string sql = "DELETE FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto conn = datasource_->get_connection();
        conn->execute(sql, { Parameter(id) });
    }

    /// Delete all
    void delete_all() {
        std::string sql = "DELETE FROM " + table_name_;
        auto conn = datasource_->get_connection();
        conn->execute(sql);
    }

    /// Save entity (Insert or Update depending on ID)
    Entity save(Entity entity) {
        auto conn = datasource_->get_connection();
        static constexpr auto members = detail::get_members<Entity>();
        
        // Find PK and check if entity is new (id == 0 or default)
        bool is_new = true;
        ID entity_id{};
        
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Id>(m)) {
                    entity_id = static_cast<ID>(entity.[:m:]);
                    constexpr bool is_gen = novaboot::di::detail::has_annotation<novaboot::annotations::GeneratedValue>(m);
                    if constexpr (is_gen) {
                        if (entity_id != 0) {
                            is_new = false;
                        }
                    } else {
                        if constexpr (std::is_integral_v<ID> || std::is_floating_point_v<ID>) {
                            if (entity_id != 0 && exists_by_id(entity_id)) {
                                is_new = false;
                            }
                        } else {
                            if (!entity_id.empty() && exists_by_id(entity_id)) {
                                is_new = false;
                            }
                        }
                    }
                }
            }
        }

        if (is_new) {
            // INSERT INTO table (col1, col2) VALUES (?, ?)
            std::string cols;
            std::string placeholders;
            std::vector<Parameter> params;
            
            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_nonstatic_data_member(m)) {
                    // Skip auto-increment ID field
                    constexpr bool is_id = novaboot::di::detail::has_annotation<novaboot::annotations::Id>(m);
                    constexpr bool is_gen = novaboot::di::detail::has_annotation<novaboot::annotations::GeneratedValue>(m);
                    
                    if constexpr (!is_id || !is_gen) {
                        if (!cols.empty()) {
                            cols += ", ";
                            placeholders += ", ";
                        }
                        cols += std::string(detail::get_member_column_name<m>().name);
                        placeholders += "?";
                        params.push_back(Parameter(entity.[:m:]));
                    }
                }
            }
            
            std::string sql = "INSERT INTO " + table_name_ + " (" + cols + ") VALUES (" + placeholders + ")";
            conn->execute(sql, params);
            
            auto last_id = conn->last_insert_id();
            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_nonstatic_data_member(m)) {
                    if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Id>(m) &&
                                  novaboot::di::detail::has_annotation<novaboot::annotations::GeneratedValue>(m)) {
                        entity.[:m:] = static_cast<decltype(entity.[:m:])>(last_id);
                    }
                }
            }
            return entity;
        } else {
            // UPDATE table SET col1 = ?, col2 = ? WHERE pk = ?
            std::string updates;
            std::vector<Parameter> params;
            
            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_nonstatic_data_member(m)) {
                    constexpr bool is_id = novaboot::di::detail::has_annotation<novaboot::annotations::Id>(m);
                    if constexpr (!is_id) {
                        if (!updates.empty()) updates += ", ";
                        updates += std::string(detail::get_member_column_name<m>().name) + " = ?";
                        params.push_back(Parameter(entity.[:m:]));
                    }
                }
            }
            
            std::string sql = "UPDATE " + table_name_ + " SET " + updates + " WHERE " + pk_col_name_ + " = ?";
            params.push_back(Parameter(entity_id));
            conn->execute(sql, params);
            return entity;
        }
    }

    /// Dynamic derived query shortcut helper
    template<auto... FieldPtrs, typename... Args>
    QueryBuilder<Entity> find_by(const Args&... args) {
        static_assert(sizeof...(FieldPtrs) == sizeof...(Args));
        auto builder = query();
        ((builder.template and_<FieldPtrs>(Op::Equal, args)), ...);
        return builder;
    }
};

} // namespace novaboot::db
