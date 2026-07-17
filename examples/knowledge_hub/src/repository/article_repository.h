#pragma once

#include "model/dto.h"
#include "model/entities.h"
#include "novaboot/db/repository.h"

#include <memory>
#include <vector>

namespace knowledge_hub::repository {

using knowledge_hub::model::Article;
using knowledge_hub::model::ArticleStatus;
using knowledge_hub::model::ArticleTitleProjection;
using knowledge_hub::model::Project;
using namespace novaboot::annotations;

struct [[= Repository() ]] ArticleRepository : public novaboot::db::CrudRepository<Article, int> {
    explicit ArticleRepository(std::shared_ptr<novaboot::db::DataSource> datasource)
        : CrudRepository<Article, int>(std::move(datasource)) {}

    ArticleRepository scoped(std::shared_ptr<novaboot::db::Connection> connection) const {
        ArticleRepository repo(datasource_);
        repo.connection_ = std::move(connection);
        return repo;
    }

    std::vector<Article> find_by_project(const Project& project) {
        return query()
            .where<&Article::project>(novaboot::db::Op::Equal, project)
            .order_by<&Article::title>()
            .list();
    }

    std::vector<ArticleTitleProjection> titles_by_project(const Project& project) {
        return query()
            .where<&Article::project>(novaboot::db::Op::Equal, project)
            .order_by<&Article::title>()
            .project<ArticleTitleProjection>();
    }

    int count_by_project(const Project& project) {
        return static_cast<int>(query()
            .where<&Article::project>(novaboot::db::Op::Equal, project)
            .count());
    }

    int count_by_status(ArticleStatus status) {
        return static_cast<int>(query()
            .where<&Article::status>(novaboot::db::Op::Equal, status)
            .count());
    }
};

} // namespace knowledge_hub::repository
