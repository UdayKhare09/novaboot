#pragma once

#include "model/entities.h"
#include "novaboot/validation/validation.h"

#include <string>
#include <vector>

namespace knowledge_hub::model {

using novaboot::validation::Schema;

struct ErrorResponse {
    std::string error;
    std::string message;
};

struct ProjectRequest {
    std::string slug;
    std::string name;
    std::string description;
    bool public_index = true;
    int review_limit = 2;

    inline static const Schema<ProjectRequest> validator =
        Schema<ProjectRequest>()
            .field<&ProjectRequest::slug>("slug").not_empty().size(2, 80)
            .field<&ProjectRequest::name>("name").not_empty().size(2, 120)
            .field<&ProjectRequest::review_limit>("review_limit").min(1).max(20);
};

struct ContributorRequest {
    std::string handle;
    std::string display_name;
    std::string role;

    inline static const Schema<ContributorRequest> validator =
        Schema<ContributorRequest>()
            .field<&ContributorRequest::handle>("handle").not_empty().size(2, 80)
            .field<&ContributorRequest::display_name>("display_name").not_empty().size(2, 120);
};

struct ArticleRequest {
    int project_id = 0;
    std::string title;
    std::string body;
    ArticleStatus status = ArticleStatus::Draft;
    int reading_minutes = 3;
    std::string topics_csv;
    std::string contributor_ids_csv;

    inline static const Schema<ArticleRequest> validator =
        Schema<ArticleRequest>()
            .field<&ArticleRequest::project_id>("project_id").min(1)
            .field<&ArticleRequest::title>("title").not_empty().size(2, 160)
            .field<&ArticleRequest::reading_minutes>("reading_minutes").min(1).max(240);
};

struct ContributorView {
    int id = 0;
    std::string handle;
    std::string display_name;
    std::string role;
};

struct ProjectView {
    int id = 0;
    std::string slug;
    std::string name;
    std::string description;
    ProjectSettings settings;
    int article_count = 0;
};

struct ArticleSummary {
    int id = 0;
    std::string title;
    ArticleStatus status = ArticleStatus::Draft;
    std::string project_name;
    int contributor_count = 0;
    int reading_minutes = 0;
};

struct ArticleDetail {
    int id = 0;
    std::string title;
    std::string body;
    ArticleStatus status = ArticleStatus::Draft;
    std::string project_name;
    ArticleMetadata metadata;
    std::vector<ContributorView> contributors;
};

struct DashboardStats {
    int projects = 0;
    int articles = 0;
    int contributors = 0;
    int published = 0;
};

struct DashboardView {
    DashboardStats stats;
    std::vector<ProjectView> projects;
    std::vector<ArticleSummary> articles;
    std::vector<ContributorView> contributors;
};

struct ArticleTitleProjection {
    int id = 0;
    std::string title;
    ArticleStatus status = ArticleStatus::Draft;
};

} // namespace knowledge_hub::model
