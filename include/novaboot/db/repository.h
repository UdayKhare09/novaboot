#pragma once
#include "novaboot/db/db_client.h"
#include "novaboot/db/exceptions.h"
#include "novaboot/db/query_builder.h"
#include "novaboot/db/orm_reflection.h"
#include "novaboot/db/transaction.h"
#include "novaboot/db/uuid.h"
#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <typeindex>
#include <type_traits>

namespace novaboot::db {

template<typename Entity, typename ID>
class CrudRepository {
protected:
    std::shared_ptr<DataSource> datasource_;
    std::shared_ptr<Connection> connection_;
    std::string table_name_;
    std::string pk_col_name_;

    std::shared_ptr<Connection> acquire_connection() const {
        if (connection_) return connection_;
        if (auto ambient = TransactionManager::current_connection_for(datasource_.get())) {
            return ambient;
        }
        return connection_ ? connection_ : datasource_->get_connection();
    }

    static consteval bool cascades_persist(novaboot::annotations::CascadeType cascade) {
        return cascade == novaboot::annotations::CascadeType::Persist ||
               cascade == novaboot::annotations::CascadeType::All;
    }

    static consteval bool cascades_merge(novaboot::annotations::CascadeType cascade) {
        return cascade == novaboot::annotations::CascadeType::Merge ||
               cascade == novaboot::annotations::CascadeType::All;
    }

    static consteval bool cascades_remove(novaboot::annotations::CascadeType cascade) {
        return cascade == novaboot::annotations::CascadeType::Remove ||
               cascade == novaboot::annotations::CascadeType::All;
    }

    template<typename Value>
    static Value read_value(ResultSet& result, int column_index) {
        if constexpr (std::is_same_v<Value, int> || std::is_same_v<Value, std::int64_t>) {
            return static_cast<Value>(result.get_int(column_index));
        } else if constexpr (std::is_same_v<Value, double> || std::is_same_v<Value, float>) {
            return static_cast<Value>(result.get_double(column_index));
        } else if constexpr (std::is_same_v<Value, std::string>) {
            return result.get_string(column_index);
        } else if constexpr (std::is_same_v<Value, Uuid>) {
            return result.get_uuid(column_index);
        } else {
            static_assert(std::is_same_v<Value, void>, "Unsupported entity identifier type");
        }
    }

    template<std::meta::info Member>
    static bool collection_loaded(Entity& entity) {
        using Field = std::remove_cvref_t<decltype(entity.[:Member:])>;
        if constexpr (detail::is_lazy_collection<Field>::value) {
            return entity.[:Member:].loaded();
        } else {
            return true;
        }
    }

    template<std::meta::info Member>
    static decltype(auto) collection_values(Entity& entity) {
        using Field = std::remove_cvref_t<decltype(entity.[:Member:])>;
        if constexpr (detail::is_lazy_collection<Field>::value) {
            return (entity.[:Member:].get());
        } else {
            return (entity.[:Member:]);
        }
    }

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

    template<std::meta::info Member>
    void remove_one_to_many_orphans(Entity& entity, const std::shared_ptr<Connection>& conn) {
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;
        using Child = typename detail::vector_value_type<Field>::type;
        using ChildId = typename detail::entity_id_type<Child>::type;
        constexpr auto one_to_many = novaboot::di::detail::get_annotation<novaboot::annotations::OneToMany>(Member);

        if (!collection_loaded<Member>(entity)) return;

        std::string placeholders;
        std::vector<Parameter> params{detail::entity_id_parameter(entity)};
        for (const auto& child : collection_values<Member>(entity)) {
            if (!placeholders.empty()) placeholders += ", ";
            placeholders += "?";
            params.push_back(detail::entity_id_parameter(child));
        }

        const auto child_table = detail::entity_table_name<Child>();
        const auto child_pk = detail::entity_primary_key_column<Child>();
        const auto join_column = detail::child_join_column_for_mapped_by<Child>(one_to_many.mapped_by);
        std::string sql = "SELECT " + child_pk + " FROM " + child_table +
                          " WHERE " + join_column + " = ?";
        if (!placeholders.empty()) {
            sql += " AND " + child_pk + " NOT IN (" + placeholders + ")";
        }

        auto dialect = datasource_->dialect();
        auto result = conn->query(dialect->convert_placeholders(sql), params);
        std::vector<ChildId> orphan_ids;
        while (result->next()) {
            orphan_ids.push_back(read_value<ChildId>(*result, 0));
        }
        result.reset();

        CrudRepository<Child, ChildId> child_repository(datasource_, conn);
        for (const auto& orphan_id : orphan_ids) {
            child_repository.delete_by_id(orphan_id);
        }
    }

    template<std::meta::info Member>
    void save_one_to_many_association(Entity& entity, const std::shared_ptr<Connection>& conn,
                                      bool parent_is_new) {
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;
        static_assert(detail::is_collection_relation_v<Field>,
                      "@OneToMany fields must be std::vector<T> or LazyCollection<T>");
        using Child = typename detail::vector_value_type<Field>::type;
        using ChildId = typename detail::entity_id_type<Child>::type;
        constexpr auto one_to_many = novaboot::di::detail::get_annotation<novaboot::annotations::OneToMany>(Member);
        static_assert(one_to_many.mapped_by[0] != '\0', "@OneToMany requires mapped_by");

        constexpr bool cascade_on_insert = cascades_persist(one_to_many.cascade);
        constexpr bool cascade_on_update = cascades_merge(one_to_many.cascade);
        if (collection_loaded<Member>(entity) &&
            ((parent_is_new && cascade_on_insert) || (!parent_is_new && cascade_on_update))) {
            CrudRepository<Child, ChildId> child_repository(datasource_, conn);
            for (auto& child : collection_values<Member>(entity)) {
                detail::set_many_to_one_reference(child, one_to_many.mapped_by, entity);
                child = child_repository.save(child);
            }
        }

        if constexpr (one_to_many.orphan_removal) {
            remove_one_to_many_orphans<Member>(entity, conn);
        }
    }

    template<std::meta::info Member>
    void save_many_to_many_association(Entity& entity, const std::shared_ptr<Connection>& conn,
                                       bool parent_is_new) {
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;
        static_assert(detail::is_collection_relation_v<Field>,
                      "@ManyToMany fields must be std::vector<T> or LazyCollection<T>");
        using Related = typename detail::vector_value_type<Field>::type;
        using RelatedId = typename detail::entity_id_type<Related>::type;
        constexpr auto many_to_many = novaboot::di::detail::get_annotation<novaboot::annotations::ManyToMany>(Member);
        static_assert(novaboot::di::detail::has_annotation<novaboot::annotations::JoinTable>(Member),
                      "@ManyToMany requires @JoinTable");
        constexpr auto join = novaboot::di::detail::get_annotation<novaboot::annotations::JoinTable>(Member);
        static_assert(join.name[0] != '\0' && join.join_column[0] != '\0' &&
                      join.inverse_join_column[0] != '\0',
                      "@JoinTable requires name, join_column, and inverse_join_column");

        constexpr bool cascade_on_insert = cascades_persist(many_to_many.cascade);
        constexpr bool cascade_on_update = cascades_merge(many_to_many.cascade);
        if (!collection_loaded<Member>(entity)) return;

        if ((parent_is_new && cascade_on_insert) || (!parent_is_new && cascade_on_update)) {
            CrudRepository<Related, RelatedId> related_repository(datasource_, conn);
            for (auto& related : collection_values<Member>(entity)) {
                related = related_repository.save(related);
            }
        }

        auto dialect = datasource_->dialect();
        conn->execute(dialect->convert_placeholders(
                          "DELETE FROM " + std::string(join.name) +
                          " WHERE " + std::string(join.join_column) + " = ?"),
                      {detail::entity_id_parameter(entity)});

        std::unordered_set<std::string> linked_related_ids;
        for (const auto& related : collection_values<Member>(entity)) {
            auto related_id = detail::entity_id_parameter(related);
            if (!linked_related_ids.insert(format_parameter(related_id)).second) {
                continue;
            }
            conn->execute(dialect->convert_placeholders(
                              "INSERT INTO " + std::string(join.name) + " (" +
                              std::string(join.join_column) + ", " +
                              std::string(join.inverse_join_column) + ") VALUES (?, ?)"),
                          {detail::entity_id_parameter(entity),
                           std::move(related_id)});
        }
    }

    template<std::meta::info Member>
    void delete_one_to_many_association(const ID& id, const std::shared_ptr<Connection>& conn) {
        using Field = std::remove_cvref_t<decltype(std::declval<Entity&>().[:Member:])>;
        static_assert(detail::is_collection_relation_v<Field>,
                      "@OneToMany fields must be std::vector<T> or LazyCollection<T>");
        using Child = typename detail::vector_value_type<Field>::type;
        using ChildId = typename detail::entity_id_type<Child>::type;
        constexpr auto one_to_many = novaboot::di::detail::get_annotation<novaboot::annotations::OneToMany>(Member);
        static_assert(one_to_many.mapped_by[0] != '\0', "@OneToMany requires mapped_by");

        if constexpr (cascades_remove(one_to_many.cascade)) {
            const auto child_table = detail::entity_table_name<Child>();
            const auto child_pk = detail::entity_primary_key_column<Child>();
            const auto join_column = detail::child_join_column_for_mapped_by<Child>(one_to_many.mapped_by);
            auto dialect = datasource_->dialect();
            auto result = conn->query(dialect->convert_placeholders(
                                          "SELECT " + child_pk + " FROM " + child_table +
                                          " WHERE " + join_column + " = ?"),
                                      {Parameter(id)});
            std::vector<ChildId> child_ids;
            while (result->next()) {
                child_ids.push_back(read_value<ChildId>(*result, 0));
            }
            result.reset();

            CrudRepository<Child, ChildId> child_repository(datasource_, conn);
            for (const auto& child_id : child_ids) {
                child_repository.delete_by_id(child_id);
            }
        }
    }

    template<std::meta::info Member>
    void delete_many_to_many_join_rows(const ID& id, const std::shared_ptr<Connection>& conn) {
        static_assert(novaboot::di::detail::has_annotation<novaboot::annotations::JoinTable>(Member),
                      "@ManyToMany requires @JoinTable");
        constexpr auto join = novaboot::di::detail::get_annotation<novaboot::annotations::JoinTable>(Member);
        auto dialect = datasource_->dialect();
        conn->execute(dialect->convert_placeholders(
                          "DELETE FROM " + std::string(join.name) +
                          " WHERE " + std::string(join.join_column) + " = ?"),
                      {Parameter(id)});
    }

    void delete_associations_by_id(const ID& id, const std::shared_ptr<Connection>& conn) {
        static constexpr auto members = detail::get_members<Entity>();
        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::OneToMany>(member)) {
                delete_one_to_many_association<member>(id, conn);
            } else if constexpr (std::meta::is_nonstatic_data_member(member) &&
                                 novaboot::di::detail::has_annotation<novaboot::annotations::ManyToMany>(member)) {
                delete_many_to_many_join_rows<member>(id, conn);
            }
        }
    }

    void save_associations(Entity& entity, const std::shared_ptr<Connection>& conn,
                           bool parent_is_new) {
        static constexpr auto members = detail::get_members<Entity>();
        template for (constexpr auto member : members) {
            if constexpr (std::meta::is_nonstatic_data_member(member) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::OneToMany>(member)) {
                save_one_to_many_association<member>(entity, conn, parent_is_new);
            } else if constexpr (std::meta::is_nonstatic_data_member(member) &&
                                 novaboot::di::detail::has_annotation<novaboot::annotations::ManyToMany>(member)) {
                save_many_to_many_association<member>(entity, conn, parent_is_new);
            }
        }
    }

public:
    explicit CrudRepository(std::shared_ptr<DataSource> ds,
                            std::shared_ptr<Connection> connection = nullptr)
        : datasource_(std::move(ds)), connection_(std::move(connection)) {
        extract_metadata();
    }

    virtual ~CrudRepository() = default;

    QueryBuilder<Entity> query() {
        auto connection = connection_;
        if (!connection) {
            connection = TransactionManager::current_connection_for(datasource_.get());
        }
        return QueryBuilder<Entity>(datasource_, table_name_, connection);
    }

    std::optional<Entity> find_by_id(const ID& id) {
        std::string sql = "SELECT " + detail::get_select_column_list<Entity>() +
                          " FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto dialect = datasource_->dialect();
        auto conn    = acquire_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), { Parameter(id) });
        if (rs->next()) {
            detail::EntityLoadContext load_context;
            load_context.datasource = datasource_.get();
            load_context.connection = conn;
            load_context.retain_connection_for_lazy = connection_ != nullptr;
            return detail::map_row_to_entity<Entity>(rs.get(), &load_context);
        }
        return std::nullopt;
    }

    bool exists_by_id(const ID& id) {
        std::string sql = "SELECT COUNT(1) FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto dialect = datasource_->dialect();
        auto conn    = acquire_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), { Parameter(id) });
        if (rs->next()) return rs->get_int(0) > 0;
        return false;
    }

    std::vector<Entity> find_all() { return query().list(); }

    Page<Entity> find_page(const Pageable& pageable) {
        return query().page(pageable);
    }

    std::vector<Entity> find_all_by_id(const std::vector<ID>& ids) {
        if (ids.empty()) return {};

        std::string placeholders;
        std::vector<Parameter> params;
        params.reserve(ids.size());
        for (const auto& id : ids) {
            if (!placeholders.empty()) placeholders += ", ";
            placeholders += "?";
            params.emplace_back(id);
        }

        std::string sql = "SELECT " + detail::get_select_column_list<Entity>() +
                          " FROM " + table_name_ + " WHERE " + pk_col_name_ +
                          " IN (" + placeholders + ")";
        auto dialect = datasource_->dialect();
        auto conn = acquire_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql), params);

        std::vector<Entity> entities;
        detail::EntityLoadContext load_context;
        load_context.datasource = datasource_.get();
        load_context.connection = conn;
        load_context.retain_connection_for_lazy = connection_ != nullptr;
        while (rs->next()) {
            entities.push_back(detail::map_row_to_entity<Entity>(rs.get(), &load_context));
        }
        return entities;
    }

    std::int64_t count() {
        std::string sql = "SELECT COUNT(1) FROM " + table_name_;
        auto dialect = datasource_->dialect();
        auto conn = acquire_connection();
        auto rs = conn->query(dialect->convert_placeholders(sql));
        return rs->next() ? rs->get_int(0) : 0;
    }

    void delete_by_id(const ID& id) {
        std::string sql = "DELETE FROM " + table_name_ + " WHERE " + pk_col_name_ + " = ?";
        auto dialect = datasource_->dialect();
        auto conn    = acquire_connection();
        delete_associations_by_id(id, conn);
        conn->execute(dialect->convert_placeholders(sql), { Parameter(id) });
    }

    void delete_entity(const Entity& entity) {
        static constexpr auto members = detail::get_members<Entity>();
        ID entity_id{};

        template for (constexpr auto m : members) {
            if constexpr (std::meta::is_nonstatic_data_member(m) &&
                          novaboot::di::detail::has_annotation<novaboot::annotations::Id>(m)) {
                entity_id = static_cast<ID>(entity.[:m:]);
            }
        }
        delete_by_id(entity_id);
    }

    void delete_all() {
        for (const auto& entity : find_all()) {
            delete_entity(entity);
        }
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

            auto conn = acquire_connection();
            std::string cols, placeholders;
            std::vector<Parameter> params;

            template for (constexpr auto m : members) {
                if constexpr (detail::is_persisted_entity_member<m>()) {
                    // @Transient — never persisted
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
                            if constexpr (novaboot::di::detail::has_annotation<ManyToOne>(m)) {
                                params.push_back(detail::entity_id_parameter(entity.[:m:]));
                            } else if constexpr (novaboot::di::detail::has_annotation<Json>(m)) {
                                if constexpr (std::is_same_v<FT, std::string>) {
                                    params.push_back(Parameter(entity.[:m:]));
                                } else {
                                    params.push_back(Parameter(novaboot::json::serialize(entity.[:m:])));
                                }
                            } else if constexpr (std::is_same_v<FT, std::chrono::system_clock::time_point> &&
                                                 novaboot::di::detail::has_annotation<Temporal>(m)) {
                                constexpr auto temporal = novaboot::di::detail::get_annotation<Temporal>(m);
                                params.push_back(Parameter(detail::temporal_to_string(entity.[:m:], temporal.value)));
                            } else if constexpr (std::is_enum_v<FT>) {
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
                            } else if constexpr (std::is_integral_v<FT> && !std::is_same_v<FT, bool>) {
                                params.push_back(Parameter(static_cast<std::int64_t>(entity.[:m:])));
                            } else if constexpr (std::is_floating_point_v<FT>) {
                                params.push_back(Parameter(static_cast<double>(entity.[:m:])));
                            } else {
                                params.push_back(Parameter(entity.[:m:]));
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

            save_associations(entity, conn, true);
            detail::invoke_lifecycle<PostPersist>(entity);
            return entity;

        // ── Step 2b: UPDATE ──────────────────────────────────────────────────
        } else {
            detail::invoke_lifecycle<PreUpdate>(entity);

            auto conn = acquire_connection();

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
                if constexpr (detail::is_persisted_entity_member<m>()) {
                    constexpr bool is_pk        = novaboot::di::detail::has_annotation<Id>(m);
                    if constexpr (!is_pk) {
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
                            if constexpr (novaboot::di::detail::has_annotation<ManyToOne>(m)) {
                                params.push_back(detail::entity_id_parameter(entity.[:m:]));
                            } else if constexpr (novaboot::di::detail::has_annotation<Json>(m)) {
                                if constexpr (std::is_same_v<FT, std::string>) {
                                    params.push_back(Parameter(entity.[:m:]));
                                } else {
                                    params.push_back(Parameter(novaboot::json::serialize(entity.[:m:])));
                                }
                            } else if constexpr (std::is_same_v<FT, std::chrono::system_clock::time_point> &&
                                                 novaboot::di::detail::has_annotation<Temporal>(m)) {
                                constexpr auto temporal = novaboot::di::detail::get_annotation<Temporal>(m);
                                params.push_back(Parameter(detail::temporal_to_string(entity.[:m:], temporal.value)));
                            } else if constexpr (std::is_enum_v<FT>) {
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
                            } else if constexpr (std::is_integral_v<FT> && !std::is_same_v<FT, bool>) {
                                params.push_back(Parameter(static_cast<std::int64_t>(entity.[:m:])));
                            } else if constexpr (std::is_floating_point_v<FT>) {
                                params.push_back(Parameter(static_cast<double>(entity.[:m:])));
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

            std::int64_t affected = 1;
            if (!updates.empty()) {
                std::string sql = "UPDATE " + table_name_ + " SET " + updates + " WHERE " + where_clause;
                params.push_back(Parameter(entity_id));
                if (current_version >= 0) {
                    params.push_back(Parameter(current_version));
                }

                auto dialect = datasource_->dialect();
                affected = conn->execute(dialect->convert_placeholders(sql), params);
            }

            // Optimistic lock conflict check: if versioned and affected == 0, conflict!
            if (current_version >= 0 && affected == 0) {
                throw OptimisticLockException(table_name_);
            }

            save_associations(entity, conn, false);
            conn.reset();
            detail::invoke_lifecycle<PostUpdate>(entity);
            return entity;
        }
    }

    std::vector<Entity> save_all(const std::vector<Entity>& entities) {
        std::vector<Entity> saved;
        saved.reserve(entities.size());
        for (const auto& entity : entities) {
            saved.push_back(save(entity));
        }
        return saved;
    }

    /// Establish an explicit persistence boundary.
    ///
    /// Repository writes are eager, unlike Hibernate's persistence context, so
    /// this currently delegates to Connection::flush(), which is a no-op for
    /// the synchronous SQLite and PostgreSQL drivers.  It exists so callers do
    /// not need to change their repository API when a buffered driver is used.
    void flush() {
        acquire_connection()->flush();
    }

    /// Dynamic derived query shortcut helper
    template<auto... FieldPtrs, typename... Args>
    QueryBuilder<Entity> find_by(const Args&... args) {
        static_assert(sizeof...(FieldPtrs) == sizeof...(Args));
        auto builder = query();
        ((builder.template and_<FieldPtrs>(Op::Equal, args)), ...);
        return builder;
    }

    template<auto FieldPtr, typename Value>
    std::optional<Entity> find_one_by(const Value& value) {
        return query().template where<FieldPtr>(Op::Equal, value).single();
    }

    template<auto FieldPtr, typename Value>
    std::vector<Entity> find_all_by(const Value& value) {
        return query().template where<FieldPtr>(Op::Equal, value).list();
    }

    template<auto FieldPtr, typename Value>
    bool exists_by(const Value& value) {
        return query().template where<FieldPtr>(Op::Equal, value).exists();
    }

    template<auto FieldPtr, typename Value>
    std::int64_t count_by(const Value& value) {
        return query().template where<FieldPtr>(Op::Equal, value).count();
    }

    template<auto FieldPtr, typename Value>
    std::int64_t delete_by(const Value& value) {
        auto matches = find_all_by<FieldPtr>(value);
        const auto deleted = static_cast<std::int64_t>(matches.size());
        for (const auto& entity : matches) {
            delete_entity(entity);
        }
        return deleted;
    }
};

} // namespace novaboot::db
