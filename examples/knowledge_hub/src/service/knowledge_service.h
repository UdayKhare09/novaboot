#pragma once

#include "model/dto.h"
#include "novaboot/db/transaction.h"
#include "repository/article_repository.h"
#include "repository/contributor_repository.h"
#include "repository/project_repository.h"

#include <chrono>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace knowledge_hub::service {

using knowledge_hub::model::Article;
using knowledge_hub::model::ArticleDetail;
using knowledge_hub::model::ArticleMetadata;
using knowledge_hub::model::ArticleRequest;
using knowledge_hub::model::ArticleStatus;
using knowledge_hub::model::ArticleSummary;
using knowledge_hub::model::Contributor;
using knowledge_hub::model::ContributorRequest;
using knowledge_hub::model::ContributorView;
using knowledge_hub::model::DashboardStats;
using knowledge_hub::model::DashboardView;
using knowledge_hub::model::Project;
using knowledge_hub::model::ProjectRequest;
using knowledge_hub::model::ProjectSettings;
using knowledge_hub::model::ProjectView;
using knowledge_hub::repository::ArticleRepository;
using knowledge_hub::repository::ContributorRepository;
using knowledge_hub::repository::ProjectRepository;
using namespace novaboot::annotations;

struct [[= Service() ]] KnowledgeService {
    std::shared_ptr<novaboot::db::DataSource> datasource;
    ProjectRepository& projects;
    ContributorRepository& contributors;
    ArticleRepository& articles;

    KnowledgeService(std::shared_ptr<novaboot::db::DataSource> ds,
                     ProjectRepository& project_repo,
                     ContributorRepository& contributor_repo,
                     ArticleRepository& article_repo)
        : datasource(std::move(ds)),
          projects(project_repo),
          contributors(contributor_repo),
          articles(article_repo) {}

    static std::string trim(std::string value) {
        auto first = value.begin();
        while (first != value.end() && std::isspace(static_cast<unsigned char>(*first))) ++first;
        auto last = value.end();
        while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;
        return std::string(first, last);
    }

    static std::vector<std::string> split_csv(std::string_view csv) {
        std::vector<std::string> values;
        std::string current;
        for (const char ch : csv) {
            if (ch == ',') {
                auto value = trim(current);
                if (!value.empty()) values.push_back(std::move(value));
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        auto value = trim(current);
        if (!value.empty()) values.push_back(std::move(value));
        return values;
    }

    static std::vector<int> split_id_csv(std::string_view csv) {
        std::vector<int> ids;
        for (const auto& value : split_csv(csv)) {
            ids.push_back(std::stoi(value));
        }
        return ids;
    }

    static ContributorView to_view(const Contributor& contributor) {
        return ContributorView{
            .id = contributor.id,
            .handle = contributor.handle,
            .display_name = contributor.display_name,
            .role = contributor.role,
        };
    }

    ProjectView to_view(const Project& project) {
        return ProjectView{
            .id = project.id,
            .slug = project.slug,
            .name = project.name,
            .description = project.description,
            .settings = project.settings,
            .article_count = articles.count_by_project(project),
        };
    }

    ArticleSummary to_summary(const Article& article) {
        return ArticleSummary{
            .id = article.id,
            .title = article.title,
            .status = article.status,
            .project_name = article.project.name,
            .contributor_count = static_cast<int>(article.contributors.size()),
            .reading_minutes = article.metadata.reading_minutes,
        };
    }

    ArticleDetail to_detail(const Article& article) {
        std::vector<ContributorView> contributor_views;
        contributor_views.reserve(article.contributors.size());
        for (const auto& contributor : article.contributors) {
            contributor_views.push_back(to_view(contributor));
        }
        return ArticleDetail{
            .id = article.id,
            .title = article.title,
            .body = article.body,
            .status = article.status,
            .project_name = article.project.name,
            .metadata = article.metadata,
            .contributors = contributor_views,
        };
    }

    DashboardView dashboard() {
        auto all_articles = articles.query().order_by<&Article::title>().list();

        DashboardView view;
        view.stats = DashboardStats{
            .projects = static_cast<int>(projects.count()),
            .articles = static_cast<int>(all_articles.size()),
            .contributors = static_cast<int>(contributors.count()),
            .published = 0,
        };

        for (const auto& project : projects.find_all()) {
            view.projects.push_back(to_view(project));
        }
        for (const auto& article : all_articles) {
            if (article.status == ArticleStatus::Published) {
                ++view.stats.published;
            }
            view.articles.push_back(to_summary(article));
        }
        for (const auto& contributor : contributors.query().order_by<&Contributor::handle>().list()) {
            view.contributors.push_back(to_view(contributor));
        }
        return view;
    }

    std::vector<ArticleSummary> articles_for_project(int project_id) {
        auto project = projects.find_by_id(project_id);
        if (!project) throw std::runtime_error("Project not found");

        std::vector<ArticleSummary> summaries;
        for (const auto& row : articles.titles_by_project(*project)) {
            summaries.push_back(ArticleSummary{
                .id = row.id,
                .title = row.title,
                .status = row.status,
                .project_name = project->name,
                .contributor_count = 0,
                .reading_minutes = 0,
            });
        }
        return summaries;
    }

    ArticleDetail article_detail(int id) {
        auto article = articles.find_by_id(id);
        if (!article) throw std::runtime_error("Article not found");
        return to_detail(*article);
    }

    ProjectView create_project(const ProjectRequest& request) {
        if (request.slug.empty() || request.name.empty()) {
            throw std::runtime_error("Project slug and name are required");
        }
        if (projects.find_by_slug(request.slug)) {
            throw std::runtime_error("Project slug already exists");
        }

        Project project;
        project.slug = request.slug;
        project.name = request.name;
        project.description = request.description;
        project.settings = ProjectSettings{
            .public_index = request.public_index,
            .review_limit = request.review_limit,
        };
        return to_view(projects.save(project));
    }

    ContributorView create_contributor(const ContributorRequest& request) {
        if (request.handle.empty() || request.display_name.empty()) {
            throw std::runtime_error("Contributor handle and display name are required");
        }
        if (contributors.find_by_handle(request.handle)) {
            throw std::runtime_error("Contributor handle already exists");
        }

        Contributor contributor;
        contributor.handle = request.handle;
        contributor.display_name = request.display_name;
        contributor.role = request.role;
        return to_view(contributors.save(contributor));
    }

    ArticleDetail create_article(const ArticleRequest& request) {
        if (request.project_id == 0 || request.title.empty()) {
            throw std::runtime_error("Project and title are required");
        }

        novaboot::db::Transaction transaction(datasource);
        auto tx_projects = projects.scoped(transaction.connection());
        auto tx_contributors = contributors.scoped(transaction.connection());
        auto tx_articles = articles.scoped(transaction.connection());

        auto project = tx_projects.find_by_id(request.project_id);
        if (!project) throw std::runtime_error("Project not found");

        Article article;
        article.project = *project;
        article.title = request.title;
        article.body = request.body;
        article.status = request.status;
        article.metadata = ArticleMetadata{
            .reading_minutes = request.reading_minutes,
            .topics = split_csv(request.topics_csv),
        };
        article.published_at = std::chrono::system_clock::now();

        for (const auto contributor_id : split_id_csv(request.contributor_ids_csv)) {
            auto contributor = tx_contributors.find_by_id(contributor_id);
            if (!contributor) throw std::runtime_error("Contributor not found");
            article.contributors.push_back(*contributor);
        }

        auto saved = tx_articles.save(article);
        transaction.commit();
        return to_detail(saved);
    }

    DashboardView seed_demo() {
        novaboot::db::Transaction transaction(datasource);
        auto tx_projects = projects.scoped(transaction.connection());
        auto tx_contributors = contributors.scoped(transaction.connection());
        auto tx_articles = articles.scoped(transaction.connection());

        tx_articles.delete_all();
        tx_contributors.delete_all();
        tx_projects.delete_all();

        Project platform;
        platform.slug = "platform";
        platform.name = "Platform Playbook";
        platform.description = "Operational notes for the NovaBoot platform team.";
        platform.settings = ProjectSettings{.public_index = true, .review_limit = 2};
        platform = tx_projects.save(platform);

        Project research;
        research.slug = "research";
        research.name = "Research Notes";
        research.description = "Experiments, architecture decisions, and technical trails.";
        research.settings = ProjectSettings{.public_index = false, .review_limit = 3};
        research = tx_projects.save(research);

        Contributor ada = tx_contributors.save(Contributor{.handle = "ada", .display_name = "Ada Lovelace", .role = "Editor"});
        Contributor grace = tx_contributors.save(Contributor{.handle = "grace", .display_name = "Grace Hopper", .role = "Reviewer"});
        Contributor linus = tx_contributors.save(Contributor{.handle = "linus", .display_name = "Linus Torvalds", .role = "Author"});

        Article onboarding;
        onboarding.project = platform;
        onboarding.title = "Postgres-backed repositories";
        onboarding.body = "This article is saved through NovaBoot repositories, hydrated with ManyToOne and ManyToMany relations, and shown in the browser UI.";
        onboarding.status = ArticleStatus::Published;
        onboarding.published_at = std::chrono::system_clock::now();
        onboarding.metadata = ArticleMetadata{.reading_minutes = 5, .topics = {"postgres", "repository", "schema"}};
        onboarding.contributors = {ada, grace};
        tx_articles.save(onboarding);

        Article migrations;
        migrations.project = research;
        migrations.title = "Schema guard and migrations";
        migrations.body = "Schema generation validates the existing tables and migrations remain explicit application code.";
        migrations.status = ArticleStatus::Review;
        migrations.published_at = std::chrono::system_clock::now();
        migrations.metadata = ArticleMetadata{.reading_minutes = 4, .topics = {"schema", "migration"}};
        migrations.contributors = {grace, linus};
        tx_articles.save(migrations);

        transaction.commit();
        return dashboard();
    }
};

} // namespace knowledge_hub::service
