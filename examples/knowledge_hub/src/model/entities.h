#pragma once

#include "novaboot/annotations/stereotypes.h"

#include <chrono>
#include <string>
#include <vector>

namespace knowledge_hub::model {

using namespace novaboot::annotations;

enum class ArticleStatus { Draft, Review, Published };

struct ProjectSettings {
    bool public_index = true;
    int review_limit = 2;
};

struct ArticleMetadata {
    int reading_minutes = 3;
    std::vector<std::string> topics;
};

struct [[= Entity("kh_articles") ]] Article;

struct [[= Entity("kh_projects") ]] Project {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= Column("slug", false, true, true, true, 80) ]]
    std::string slug;

    [[= Column("name", false, true, true, false, 160) ]]
    std::string name;

    std::string description;

    [[= Json() ]]
    ProjectSettings settings;

    [[= OneToMany("project", FetchType::Eager, CascadeType::All, true) ]]
    std::vector<Article> articles;
};

struct [[= Entity("kh_contributors") ]] Contributor {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= Column("handle", false, true, true, true, 80) ]]
    std::string handle;

    std::string display_name;
    std::string role;
};

struct [[= Entity("kh_articles") ]] Article {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= Column("title", false, true, true, false, 180) ]]
    std::string title;

    std::string body;

    [[= Enumerated(EnumType::String) ]]
    ArticleStatus status = ArticleStatus::Draft;

    [[= Temporal(TemporalType::Timestamp) ]]
    std::chrono::system_clock::time_point published_at;

    [[= Json() ]]
    ArticleMetadata metadata;

    [[= ManyToOne() ]]
    [[= JoinColumn("project_id") ]]
    Project project;

    [[= ManyToMany(FetchType::Eager) ]]
    [[= JoinTable("kh_article_contributors", "article_id", "contributor_id") ]]
    std::vector<Contributor> contributors;
};

} // namespace knowledge_hub::model
