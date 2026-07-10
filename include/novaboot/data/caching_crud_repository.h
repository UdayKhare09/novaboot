#pragma once

#include "novaboot/data/crud_repository.h"
#include "novaboot/data/cache_repository.h"
#include "novaboot/data/repository_utils.h"
#include <chrono>
#include <spdlog/spdlog.h>

namespace novaboot::data {

template<typename Entity, typename Id>
class CachingCrudRepository : public CrudRepository<Entity, Id> {
public:
    CachingCrudRepository(CrudRepository<Entity, Id>& sql_repo,
                          CacheRepository<Entity, Id>& cache_repo,
                          std::chrono::seconds ttl = std::chrono::seconds(60))
        : sql_repo_(&sql_repo),
          cache_repo_(&cache_repo),
          ttl_(ttl) {}

    std::optional<Entity> find_by_id(const Id& id) override {
        try {
            auto cached = cache_repo_->get(id);
            if (cached) {
                spdlog::info("CachingCrudRepository: Cache HIT for id: {}", id);
                return cached;
            }
        } catch (...) {
            spdlog::warn("CachingCrudRepository: Cache exception while getting id: {}", id);
        }

        spdlog::info("CachingCrudRepository: Cache MISS for id: {}", id);
        auto db_res = sql_repo_->find_by_id(id);
        if (db_res) {
            try {
                cache_repo_->put(id, *db_res, ttl_);
                spdlog::info("CachingCrudRepository: Cache POPULATED for id: {}", id);
            } catch (...) {
                spdlog::warn("CachingCrudRepository: Cache exception while putting id: {}", id);
            }
        }
        return db_res;
    }

    std::vector<Entity> find_all() override {
        return sql_repo_->find_all();
    }

    Entity save(const Entity& e) override {
        auto saved = sql_repo_->save(e);
        try {
            auto id = detail::get_entity_id(saved);
            cache_repo_->evict(id);
            spdlog::info("CachingCrudRepository: Cache EVICTED for id: {}", id);
        } catch (...) {
            // ignore evict failure
        }
        return saved;
    }

    void delete_by_id(const Id& id) override {
        sql_repo_->delete_by_id(id);
        try {
            cache_repo_->evict(id);
            spdlog::info("CachingCrudRepository: Cache EVICTED for id: {}", id);
        } catch (...) {
            // ignore evict failure
        }
    }

    bool exists_by_id(const Id& id) override {
        try {
            if (cache_repo_->exists(id)) return true;
        } catch (...) {
            // ignore cache check failure
        }
        return sql_repo_->exists_by_id(id);
    }

    std::size_t count() override {
        return sql_repo_->count();
    }

protected:
    CrudRepository<Entity, Id>* sql_repo_;
    CacheRepository<Entity, Id>* cache_repo_;
    std::chrono::seconds ttl_;
};

} // namespace novaboot::data
