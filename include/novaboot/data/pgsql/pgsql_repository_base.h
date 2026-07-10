#pragma once

#include "novaboot/data/crud_repository.h"
#include "novaboot/data/pgsql/pgsql_data_source.h"
#include "novaboot/data/repository_utils.h" // for get_entity_id helper

namespace novaboot::data {

template<typename Entity, typename Id>
class PgsqlRepositoryBase : public CrudRepository<Entity, Id> {
public:
    explicit PgsqlRepositoryBase(PgsqlDataSource& ds) : ds_(ds) {}

    std::optional<Entity> find_by_id(const Id& id) override {
        return ds_.transact([&](auto& db) -> std::optional<Entity> {
            try {
                auto ptr = db.template find<Entity>(id);
                if (ptr) return *ptr;
                return std::nullopt;
            } catch (...) {
                return std::nullopt;
            }
        });
    }

    std::vector<Entity> find_all() override {
        return ds_.transact([&](auto& db) {
            std::vector<Entity> result;
            auto r = db.template query<Entity>();
            for (auto& e : r) {
                result.push_back(e);
            }
            return result;
        });
    }

    Entity save(const Entity& e) override {
        return ds_.transact([&](auto& db) {
            try {
                auto id = detail::get_entity_id(e);
                if constexpr (std::is_integral_v<Id>) {
                    if (id == 0) {
                        Entity mutable_e = e;
                        db.persist(mutable_e);
                        return mutable_e;
                    }
                }
                
                auto ptr = db.template find<Entity>(id);
                if (ptr) {
                    db.update(e);
                    return e;
                } else {
                    Entity mutable_e = e;
                    db.persist(mutable_e);
                    return mutable_e;
                }
            } catch (...) {
                Entity mutable_e = e;
                db.persist(mutable_e);
                return mutable_e;
            }
        });
    }

    void delete_by_id(const Id& id) override {
        ds_.transact([&](auto& db) {
            db.template erase<Entity>(id);
        });
    }

    bool exists_by_id(const Id& id) override {
        return ds_.transact([&](auto& db) {
            auto ptr = db.template find<Entity>(id);
            return ptr != nullptr;
        });
    }

    std::size_t count() override {
        return ds_.transact([&](auto& db) {
            auto r = db.template query<Entity>();
            return static_cast<std::size_t>(r.size());
        });
    }

protected:
    PgsqlDataSource& ds_;
};

} // namespace novaboot::data
