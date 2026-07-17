#pragma once

#include "model/entities.h"
#include "novaboot/db/repository.h"

#include <memory>
#include <string>

namespace knowledge_hub::repository {

using knowledge_hub::model::Project;
using namespace novaboot::annotations;

struct [[= Repository() ]] ProjectRepository : public novaboot::db::CrudRepository<Project, int> {
    explicit ProjectRepository(std::shared_ptr<novaboot::db::DataSource> datasource)
        : CrudRepository<Project, int>(std::move(datasource)) {}

    ProjectRepository scoped(std::shared_ptr<novaboot::db::Connection> connection) const {
        ProjectRepository repo(datasource_);
        repo.connection_ = std::move(connection);
        return repo;
    }

    std::optional<Project> find_by_slug(const std::string& slug) {
        return query().where<&Project::slug>(novaboot::db::Op::Equal, slug).single();
    }
};

} // namespace knowledge_hub::repository
