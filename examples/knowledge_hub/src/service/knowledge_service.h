#pragma once

#include "model/dto.h"
#include "repository/article_repository.h"
#include "repository/contributor_repository.h"
#include "repository/project_repository.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace knowledge_hub::service {

using knowledge_hub::model::Article;
using knowledge_hub::model::ArticleDetail;
using knowledge_hub::model::ArticleMetadata;
using knowledge_hub::model::ArticlePageQuery;
using knowledge_hub::model::ArticlePageView;
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
    ProjectRepository& projects;
    ContributorRepository& contributors;
    ArticleRepository& articles;

    KnowledgeService(ProjectRepository& project_repo,
                     ContributorRepository& contributor_repo,
                     ArticleRepository& article_repo)
        : projects(project_repo),
          contributors(contributor_repo),
          articles(article_repo) {}

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
            .article_count = static_cast<int>(project.articles.count()),
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

    DashboardView dashboard_with(ProjectRepository& project_repo,
                                 ContributorRepository& contributor_repo,
                                 ArticleRepository& article_repo) {
        auto all_articles = article_repo.query()
            .fetch<&Article::contributors>()
            .order_by<&Article::title>()
            .list();
        auto all_projects = project_repo.query()
            .fetch<&Project::articles>()
            .order_by<&Project::name>()
            .list();

        DashboardView view;
        view.stats = DashboardStats{
            .projects = static_cast<int>(project_repo.count()),
            .articles = static_cast<int>(all_articles.size()),
            .contributors = static_cast<int>(contributor_repo.count()),
            .published = 0,
        };

        for (const auto& project : all_projects) {
            view.projects.push_back(to_view(project));
        }
        for (const auto& article : all_articles) {
            if (article.status == ArticleStatus::Published) {
                ++view.stats.published;
            }
            view.articles.push_back(to_summary(article));
        }
        for (const auto& contributor : contributor_repo.query().order_by<&Contributor::handle>().list()) {
            view.contributors.push_back(to_view(contributor));
        }
        return view;
    }

    DashboardView dashboard() {
        return dashboard_with(projects, contributors, articles);
    }

    ArticlePageView article_page(const ArticlePageQuery& query) {
        auto page = articles.query()
            .fetch<&Article::contributors>()
            .page(novaboot::db::Pageable::of(query.page, query.size)
                .sort_by<Article, &Article::published_at>(false)
                .sort_by<Article, &Article::title>());

        ArticlePageView view;
        view.page = page.page;
        view.size = page.size;
        view.total_pages = static_cast<int>(page.total_pages());
        view.total_elements = static_cast<int>(page.total_elements);
        view.number_of_elements = static_cast<int>(page.number_of_elements());
        view.first = page.is_first();
        view.last = page.is_last();
        view.has_next = page.has_next();
        view.has_previous = page.has_previous();
        view.content.reserve(page.content.size());
        for (const auto& article : page.content) {
            view.content.push_back(to_summary(article));
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
        auto article = articles.query()
            .fetch<&Article::contributors>()
            .where<&Article::id>(novaboot::db::Op::Equal, id)
            .single();
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
        project.settings = request.settings;
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

    [[= Transactional() ]]
    ArticleDetail create_article(const ArticleRequest& request) {
        if (request.project_id == 0 || request.title.empty()) {
            throw std::runtime_error("Project and title are required");
        }

        auto project = projects.find_by_id(request.project_id);
        if (!project) throw std::runtime_error("Project not found");

        Article article;
        article.project = *project;
        article.title = request.title;
        article.body = request.body;
        article.status = request.status;
        article.metadata = request.metadata;
        article.published_at = std::chrono::system_clock::now();

        std::vector<Contributor> selected_contributors;
        for (const auto contributor_id : request.contributor_ids) {
            auto contributor = contributors.find_by_id(contributor_id);
            if (!contributor) throw std::runtime_error("Contributor not found");
            selected_contributors.push_back(*contributor);
        }
        article.contributors =
            novaboot::db::LazyCollection<Contributor>::loaded(std::move(selected_contributors));

        auto saved = articles.save(article);
        return to_detail(saved);
    }

    [[= Transactional() ]]
    DashboardView seed_demo() {
        articles.delete_all();
        contributors.delete_all();
        projects.delete_all();

        Project platform;
        platform.slug = "platform";
        platform.name = "Platform Playbook";
        platform.description = "Operational notes for the NovaBoot platform team.";
        platform.settings = ProjectSettings{.public_index = true, .review_limit = 2};
        platform = projects.save(platform);

        Project research;
        research.slug = "research";
        research.name = "Research Notes";
        research.description = "Experiments, architecture decisions, and technical trails.";
        research.settings = ProjectSettings{.public_index = false, .review_limit = 3};
        research = projects.save(research);

        Contributor ada = contributors.save(Contributor{.handle = "ada", .display_name = "Ada Lovelace", .role = "Editor"});
        Contributor grace = contributors.save(Contributor{.handle = "grace", .display_name = "Grace Hopper", .role = "Reviewer"});
        Contributor linus = contributors.save(Contributor{.handle = "linus", .display_name = "Linus Torvalds", .role = "Author"});

        Article onboarding;
        onboarding.project = platform;
        onboarding.title = "Postgres-backed repositories";
        onboarding.body = "This article is saved through NovaBoot repositories, hydrated with ManyToOne and ManyToMany relations, and shown in the browser UI.";
        onboarding.status = ArticleStatus::Published;
        onboarding.published_at = std::chrono::system_clock::now();
        onboarding.metadata = ArticleMetadata{.reading_minutes = 5, .topics = {"postgres", "repository", "schema"}};
        onboarding.contributors = novaboot::db::LazyCollection<Contributor>::loaded({ada, grace});
        articles.save(onboarding);

        Article migrations;
        migrations.project = research;
        migrations.title = "Schema guard and migrations";
        migrations.body = "Schema generation validates the existing tables and migrations remain explicit application code.";
        migrations.status = ArticleStatus::Review;
        migrations.published_at = std::chrono::system_clock::now();
        migrations.metadata = ArticleMetadata{.reading_minutes = 4, .topics = {"schema", "migration"}};
        migrations.contributors = novaboot::db::LazyCollection<Contributor>::loaded({grace, linus});
        articles.save(migrations);

        return dashboard();
    }
};

} // namespace knowledge_hub::service
