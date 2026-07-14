#pragma once
#include "novaboot/db/db_client.h"
#include "novaboot/db/exceptions.h"
#include "novaboot/db/query_builder.h"
#include "novaboot/db/orm_reflection.h"
#include "novaboot/db/uuid.h"
#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <typeindex>

namespace novaboot::db {

template<typename Entity, typename ID>
class CrudRepository {
protected:
    std::shared_ptr<DataSource> datasource_;
    std::string table_name_;
    std::string pk_col_name_;

    void extract_metadata() {
        using namespace novaboot::annotations;

        // Table name priority: @Table(name) > @Entity(name) > snake_case plural
        if constexpr (novaboot::di::detail::has_annotation<Table>(^^Entity)) {
            constexpr auto tbl = novaboot::di::detail::get_annotation<Table>(^^Entity);
            if constexpr (tbl.name[0] != '\0') {
                if constexpr (tbl.schema[0] != '\0') {
                    table_name_ = std::string(tbl.schema) + "." + std::string(tbl.name);
                } else {
                    table_name_ = std::string(tbl.name);
                }
            }
        }

        if (table_name_.empty()) {
            constexpr auto ent = detail::get_table_metadata<Entity>();
            if constexpr (ent.name[0] != '\0') {
                table_name_ = ent.name;
            } else {
                constexpr auto raw_name = std::meta::identifier_of(^^Entity);
                table_name_ = detail::to_snake_case(raw_name) + "s";
            }
        }

        // Discover primary key column from @Id annotation
        static constexpr auto members = detail::get_members<Entity>();
        pk_col_name_ = "id";

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

    QueryBuilder<Entity> query() {
        return QueryBuilder<Entity>(datasource_, table_name_);
    }

    std::optional<Entity> find_by_id(const ID& id) {
        std::string sql = "SELECT * FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto dialect = datasource_->dialect();
        auto conn    = datasource_->get_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), { Parameter(id) });
        if (rs->next()) {
            return detail::map_row_to_entity<Entity>(rs.get());
        }
        return std::nullopt;
    }

    bool exists_by_id(const ID& id) {
        std::string sql = "SELECT COUNT(1) FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto dialect = datasource_->dialect();
        auto conn    = datasource_->get_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), { Parameter(id) });
        if (rs->next()) return rs->get_int(0) > 0;
        return false;
    }

    std::vector<Entity> find_all() { return query().list(); }

    void delete_by_id(const ID& id) {
        std::string sql = "DELETE FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto dialect = datasource_->dialect();
        auto conn    = datasource_->get_connection();
        conn->execute(dialect->convert_placeholders(sql), { Parameter(id) });
    }

    void delete_all() {
        std::string sql = "DELETE FROM " + table_name_;
        auto dialect = datasource_->dialect();
        auto conn    = datasource_->get_connection();
        conn->execute(dialect->convert_placeholders(sql));
    }

    // -----------------------------------------------------------------------
    // save() — Insert or Update.
    //
    // Annotation behaviour:
    //   @Transient             — excluded from all SQL
    //   @Column(insertable)    — excluded from INSERT when false
    //   @Column(updatable)     — excluded from UPDATE SET when false
    //   @Version               — init=1 on INSERT; check+increment+lock on UPDATE
    //   @PrePersist/@PostPersist   — hooks around INSERT
    //   @PreUpdate/@PostUpdate     — hooks around UPDATE
    // -----------------------------------------------------------------------
    Entity save(Entity entity) {
        using namespace novaboot::annotations;
        static constexpr auto members = detail::get_members<Entity>();

        bool is_new = true;
        ID entity_id{};

        // ── Step 1: discover ID value and decide insert vs update ────────────
        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m)) {
                if constexpr (novaboot::di::detail::has_annotation<Id>(m)) {
                    entity_id = static_cast<ID>(entity.[:m:]);

                    constexpr bool is_gen = novaboot::di::detail::has_annotation<GeneratedValue>(m);

                    if constexpr (is_gen) {
                        constexpr auto gen_anno = novaboot::di::detail::get_annotation<GeneratedValue>(m);

                        if constexpr (gen_anno.strategy == GenerationType::UUID) {
                            using FT = decltype(entity.[:m:]);
                            bool is_empty = false;
                            if constexpr (std::is_same_v<FT, Uuid>)        is_empty = entity.[:m:].is_nil();
                            else if constexpr (std::is_same_v<FT, std::string>) is_empty = entity.[:m:].empty();

                            if (is_empty) {
                                if constexpr (std::is_same_v<FT, Uuid>) {
                                    entity.[:m:] = Uuid::generate();
                                } else if constexpr (std::is_same_v<FT, std::string>) {
                                    entity.[:m:] = Uuid::generate().to_string();
                                }
                                entity_id = static_cast<ID>(entity.[:m:]);
                                is_new = true;
                            } else {
                                bool already_exists = false;
                                if constexpr (std::is_integral_v<ID> || std::is_floating_point_v<ID>)
                                    already_exists = (entity_id != 0) && exists_by_id(entity_id);
                                else if constexpr (std::is_same_v<ID, Uuid>)
                                    already_exists = !entity_id.is_nil() && exists_by_id(entity_id);
                                else
                                    already_exists = !entity_id.empty() && exists_by_id(entity_id);
                                if (already_exists) is_new = false;
                            }
                        } else {
                            // AutoIncrement
                            if (entity_id != 0) is_new = false;
                        }
                    } else {
                        // No @GeneratedValue
                        bool already_exists = false;
                        if constexpr (std::is_integral_v<ID> || std::is_floating_point_v<ID>)
                            already_exists = (entity_id != 0) && exists_by_id(entity_id);
                        else if constexpr (std::is_same_v<ID, Uuid>)
                            already_exists = !entity_id.is_nil() && exists_by_id(entity_id);
                        else
                            already_exists = !entity_id.empty() && exists_by_id(entity_id);
                        if (already_exists) is_new = false;
                    }
                }
            }
        }

        // ── Step 2a: INSERT ──────────────────────────────────────────────────
        if (is_new) {
            detail::invoke_lifecycle<PrePersist>(entity);

            auto conn = datasource_->get_connection();
            std::string cols, placeholders;
            std::vector<Parameter> params;

            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_nonstatic_data_member(m)) {
                    // @Transient — never persisted
                    constexpr bool is_transient = novaboot::di::detail::has_annotation<Transient>(m);
                    if constexpr (!is_transient) {
                        constexpr bool is_id      = novaboot::di::detail::has_annotation<Id>(m);
                        constexpr bool is_gen_val = novaboot::di::detail::has_annotation<GeneratedValue>(m);
                        // Skip AutoIncrement PK — DB provides it
                        constexpr bool is_auto_inc = is_id && is_gen_val && (
                            novaboot::di::detail::get_annotation<GeneratedValue>(m).strategy == GenerationType::AutoIncrement);

                        if constexpr (!is_auto_inc) {
                            // Check @Column(insertable=false)
                            constexpr bool has_col   = novaboot::di::detail::has_annotation<Column>(m);
                            constexpr bool insertable = !has_col ||
                                novaboot::di::detail::get_annotation<Column>(m).insertable;

                            if constexpr (insertable) {
                                // @Version — initialise to 1 on INSERT
                                if constexpr (novaboot::di::detail::has_annotation<Version>(m)) {
                                    entity.[:m:] = static_cast<decltype(entity.[:m:])>(1);
                                }
                                if (!cols.empty()) { cols += ", "; placeholders += ", "; }
                                cols += std::string(detail::get_member_column_name<m>().name);
                                placeholders += "?";

                                using FT = std::remove_cvref_t<decltype(entity.[:m:])>;
                                if constexpr (std::is_enum_v<FT>) {
                                    if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Enumerated>(m)) {
                                        constexpr auto en = novaboot::di::detail::get_annotation<novaboot::annotations::Enumerated>(m);
                                        if constexpr (en.value == novaboot::annotations::EnumType::Ordinal) {
                                            params.push_back(Parameter(static_cast<std::int64_t>(entity.[:m:])));
                                        } else {
                                            static constexpr auto enumerators = detail::get_enumerators<FT>();
                                            std::string enum_str;
                                            template for (constexpr auto e : enumerators) {
                                                if ([:e:] == entity.[:m:]) {
                                                    enum_str = std::string(std::meta::identifier_of(e));
                                                }
                                            }
                                            params.push_back(Parameter(enum_str));
                                        }
                                    } else {
                                        params.push_back(Parameter(static_cast<std::int64_t>(entity.[:m:])));
                                    }
                                } else {
                                    params.push_back(Parameter(entity.[:m:]));
                                }
                            }
                        }
                    }
                }
            }

            std::string sql = "INSERT INTO " + table_name_ + " (" + cols + ") VALUES (" + placeholders + ")";
            auto dialect = datasource_->dialect();
            conn->execute(dialect->convert_placeholders(sql), params);

            // Read back AutoIncrement PK
            constexpr bool has_auto_inc = []() consteval {
                bool found = false;
                template for (constexpr auto m : members) {
                    if constexpr (std::meta::is_nonstatic_data_member(m)) {
                        if constexpr (novaboot::di::detail::has_annotation<Id>(m) &&
                                      novaboot::di::detail::has_annotation<GeneratedValue>(m)) {
                            if constexpr (novaboot::di::detail::get_annotation<GeneratedValue>(m).strategy
                                          == GenerationType::AutoIncrement) {
                                found = true;
                            }
                        }
                    }
                }
                return found;
            }();

            if constexpr (has_auto_inc) {
                auto last_id = conn->last_insert_id();
                template for (constexpr auto m : members) {
                    if constexpr (std::meta::is_nonstatic_data_member(m) &&
                                  novaboot::di::detail::has_annotation<Id>(m) &&
                                  novaboot::di::detail::has_annotation<GeneratedValue>(m)) {
                        if constexpr (novaboot::di::detail::get_annotation<GeneratedValue>(m).strategy
                                      == GenerationType::AutoIncrement) {
                            entity.[:m:] = static_cast<decltype(entity.[:m:])>(last_id);
                        }
                    }
                }
            }

            detail::invoke_lifecycle<PostPersist>(entity);
            return entity;

        // ── Step 2b: UPDATE ──────────────────────────────────────────────────
        } else {
            detail::invoke_lifecycle<PreUpdate>(entity);

            auto conn = datasource_->get_connection();

            // Capture current @Version for WHERE optimistic-lock clause
            std::int64_t current_version = -1;
            std::string  version_col;

            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_nonstatic_data_member(m)) {
                    if constexpr (novaboot::di::detail::has_annotation<Version>(m)) {
                        current_version = static_cast<std::int64_t>(entity.[:m:]);
                        version_col     = std::string(detail::get_member_column_name<m>().name);
                    }
                }
            }

            std::string updates;
            std::vector<Parameter> params;

            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_nonstatic_data_member(m)) {
                    constexpr bool is_pk        = novaboot::di::detail::has_annotation<Id>(m);
                    constexpr bool is_transient = novaboot::di::detail::has_annotation<Transient>(m);
                    if constexpr (!is_pk && !is_transient) {
                        constexpr bool has_col    = novaboot::di::detail::has_annotation<Column>(m);
                        constexpr bool updatable  = !has_col ||
                            novaboot::di::detail::get_annotation<Column>(m).updatable;

                        if constexpr (updatable) {
                            // @Version — increment before setting
                            if constexpr (novaboot::di::detail::has_annotation<Version>(m)) {
                                entity.[:m:] = static_cast<decltype(entity.[:m:])>(current_version + 1);
                            }
                            if (!updates.empty()) updates += ", ";
                            updates += std::string(detail::get_member_column_name<m>().name) + " = ?";
                            
                            using FT = std::remove_cvref_t<decltype(entity.[:m:])>;
                            if constexpr (std::is_enum_v<FT>) {
                                if constexpr (novaboot::di::detail::has_annotation<novaboot::annotations::Enumerated>(m)) {
                                    constexpr auto en = novaboot::di::detail::get_annotation<novaboot::annotations::Enumerated>(m);
                                    if constexpr (en.value == novaboot::annotations::EnumType::Ordinal) {
                                        params.push_back(Parameter(static_cast<std::int64_t>(entity.[:m:])));
                                    } else {
                                        static constexpr auto enumerators = detail::get_enumerators<FT>();
                                        std::string enum_str;
                                        template for (constexpr auto e : enumerators) {
                                            if ([:e:] == entity.[:m:]) {
                                                enum_str = std::string(std::meta::identifier_of(e));
                                            }
                                        }
                                        params.push_back(Parameter(enum_str));
                                    }
                                } else {
                                    params.push_back(Parameter(static_cast<std::int64_t>(entity.[:m:])));
                                }
                            } else {
                                params.push_back(Parameter(entity.[:m:]));
                            }
                        }
                    }
                }
            }

            // WHERE pk = ? [AND version = ?]
            std::string where_clause = pk_col_name_ + " = ?";
            if (current_version >= 0) {
                where_clause += " AND " + version_col + " = ?";
            }

            std::string sql = "UPDATE " + table_name_ + " SET " + updates + " WHERE " + where_clause;
            params.push_back(Parameter(entity_id));
            if (current_version >= 0) {
                params.push_back(Parameter(current_version));
            }

            auto dialect = datasource_->dialect();
            auto affected = conn->execute(dialect->convert_placeholders(sql), params);
            conn.reset();

            // Optimistic lock conflict check: if versioned and affected == 0, conflict!
            if (current_version >= 0 && affected == 0) {
                throw OptimisticLockException(table_name_);
            }

            detail::invoke_lifecycle<PostUpdate>(entity);
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
