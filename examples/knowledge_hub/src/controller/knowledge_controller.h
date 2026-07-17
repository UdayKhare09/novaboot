#pragma once

#include "model/dto.h"
#include "novaboot/router/response_entity.h"
#include "service/knowledge_service.h"

#include <vector>

namespace knowledge_hub::controller {

using knowledge_hub::model::ArticleDetail;
using knowledge_hub::model::ArticlePageQuery;
using knowledge_hub::model::ArticlePageView;
using knowledge_hub::model::ArticleRequest;
using knowledge_hub::model::ArticleSummary;
using knowledge_hub::model::ContributorRequest;
using knowledge_hub::model::ContributorView;
using knowledge_hub::model::DashboardView;
using knowledge_hub::model::ProjectRequest;
using knowledge_hub::model::ProjectView;
using knowledge_hub::service::KnowledgeService;
using namespace novaboot::annotations;

struct [[= RestController("/api") ]] KnowledgeController {
    KnowledgeService& service;

    explicit KnowledgeController(KnowledgeService& knowledge_service)
        : service(knowledge_service) {}

    [[= GetMapping("/dashboard") ]]
    novaboot::ResponseEntity<DashboardView> dashboard() {
        return novaboot::ResponseEntity<DashboardView>::ok(service.dashboard());
    }

    [[= GetMapping("/projects/:id/articles") ]]
    novaboot::ResponseEntity<std::vector<ArticleSummary>> project_articles(std::string id) {
        return novaboot::ResponseEntity<std::vector<ArticleSummary>>::ok(
            service.articles_for_project(std::stoi(id)));
    }

    [[= GetMapping("/articles/:id") ]]
    novaboot::ResponseEntity<ArticleDetail> article(std::string id) {
        return novaboot::ResponseEntity<ArticleDetail>::ok(service.article_detail(std::stoi(id)));
    }

    [[= GetMapping("/articles") ]]
    novaboot::ResponseEntity<ArticlePageView> articles(ArticlePageQuery query) {
        return novaboot::ResponseEntity<ArticlePageView>::ok(service.article_page(query));
    }

    [[= PostMapping("/projects") ]]
    novaboot::ResponseEntity<ProjectView> create_project(ProjectRequest request) {
        return novaboot::ResponseEntity<ProjectView>::status(201, service.create_project(request));
    }

    [[= PostMapping("/contributors") ]]
    novaboot::ResponseEntity<ContributorView> create_contributor(ContributorRequest request) {
        return novaboot::ResponseEntity<ContributorView>::status(201, service.create_contributor(request));
    }

    [[= PostMapping("/articles") ]]
    novaboot::ResponseEntity<ArticleDetail> create_article(ArticleRequest request) {
        return novaboot::ResponseEntity<ArticleDetail>::status(201, service.create_article(request));
    }

    [[= PostMapping("/seed") ]]
    novaboot::ResponseEntity<DashboardView> seed() {
        return novaboot::ResponseEntity<DashboardView>::ok(service.seed_demo());
    }
};

} // namespace knowledge_hub::controller
