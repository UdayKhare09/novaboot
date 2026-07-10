#pragma once

#include <optional>
#include <vector>
#include <cstddef>

namespace novaboot::data {

template<typename Entity, typename Id>
struct CrudRepository {
    virtual std::optional<Entity> find_by_id(const Id& id) = 0;
    virtual std::vector<Entity>   find_all()                = 0;
    virtual Entity                save(const Entity& e)     = 0;
    virtual void                  delete_by_id(const Id& id)= 0;
    virtual bool                  exists_by_id(const Id& id)= 0;
    virtual std::size_t           count()                   = 0;
    virtual ~CrudRepository() = default;
};

} // namespace novaboot::data
