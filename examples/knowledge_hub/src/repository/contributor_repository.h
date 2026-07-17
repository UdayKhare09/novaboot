#pragma once

#include "model/entities.h"
#include "novaboot/db/repository.h"

#include <memory>
#include <string>

namespace knowledge_hub::repository {

using knowledge_hub::model::Contributor;
using namespace novaboot::annotations;

struct [[= Repository() ]] ContributorRepository : public novaboot::db::CrudRepository<Contributor, int> {
    explicit ContributorRepository(std::shared_ptr<novaboot::db::DataSource> datasource)
        : CrudRepository<Contributor, int>(std::move(datasource)) {}

    ContributorRepository scoped(std::shared_ptr<novaboot::db::Connection> connection) const {
        ContributorRepository repo(datasource_);
        repo.connection_ = std::move(connection);
        return repo;
    }

    std::optional<Contributor> find_by_handle(const std::string& handle) {
        return query().where<&Contributor::handle>(novaboot::db::Op::Equal, handle).single();
    }
};

} // namespace knowledge_hub::repository
