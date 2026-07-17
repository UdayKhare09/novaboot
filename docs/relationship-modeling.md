# Relationship modelling examples

NovaBoot relationship mapping follows the shape of JPA annotations, but the
runtime behaviour is deliberately C++-native: ownership is explicit, lazy
loading uses explicit holder types, and transactions are plain service-layer
boundaries.

## Required `@ManyToOne`

```cpp
struct [[= Entity("articles") ]] Article {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string title;

    [[= ManyToOne() ]]
    [[= JoinColumn("project_id") ]]
    Project project;
};
```

The article table owns the `project_id` foreign key. Persisting an article
writes the project id into that column.

## Nullable `@ManyToOne`

```cpp
struct [[= Entity("articles") ]] Article {
    [[= Id() ]]
    int id = 0;

    [[= ManyToOne() ]]
    [[= JoinColumn("reviewer_id") ]]
    std::optional<Contributor> reviewer;
};
```

Use `std::optional<T>` for nullable to-one relationships. `std::nullopt`
persists as SQL `NULL` and loads back as `std::nullopt`.

## Lazy to-one

```cpp
struct [[= Entity("articles") ]] Article {
    [[= Id() ]]
    int id = 0;

    [[= ManyToOne(FetchType::Lazy) ]]
    [[= JoinColumn("project_id") ]]
    novaboot::db::Lazy<Project> project;
};
```

`Lazy<T>` stores the related id and loads the entity only when `get()`, `->`, or
`*` is used. The first load is cached. For work that crosses several lazy
relations, prefer a transaction boundary so the lazy loads share the intended
database context.

## Inverse `@OneToMany`

```cpp
struct [[= Entity("projects") ]] Project {
    [[= Id() ]]
    int id = 0;

    [[= OneToMany("project", CascadeType::All, true) ]]
    std::vector<Article> articles;
};
```

`@OneToMany` is inverse-side only. The child still owns the database foreign key
through its `@ManyToOne` field. With `orphan_removal=true`, removing an existing
child from the collection deletes that child row on save.

## `@ManyToMany`

```cpp
struct [[= Entity("articles") ]] Article {
    [[= Id() ]]
    int id = 0;

    [[= ManyToMany(FetchType::Lazy) ]]
    [[= JoinTable("article_contributors", "article_id", "contributor_id") ]]
    novaboot::db::LazyCollection<Contributor> contributors;
};
```

`@ManyToMany` requires an explicit join table. Saving the owner rewrites join
rows. Duplicate related ids are collapsed before inserts. Deleting the owner
cleans join rows but does not delete shared contributor rows.

## Fetching relationships

Use fetch hints when a read path knows it needs a relationship immediately:

```cpp
auto articles = article_repository.query()
    .where<&Article::status>(novaboot::db::Op::Equal, ArticleStatus::Published)
    .fetch<&Article::contributors>()
    .list();
```

For to-one relations, fetch joins hydrate the relation as part of the list
query. For collections, NovaBoot loads the requested collection after loading
the parent rows.

## Cascades and transactions

Cascade rules are intentionally conservative:

- `Persist` cascades saves from new owners.
- `Merge` cascades saves from existing owners.
- `Remove` cascades deletes for supported one-to-many child ownership.
- `All` enables the above where supported.
- Many-to-many remove cleanup stops at the join table.

Wrap multi-step writes in a service transaction:

```cpp
return transactions.execute([&](std::shared_ptr<novaboot::db::Connection>) {
    auto project = projects.find_by_id(project_id);
    if (!project) throw std::runtime_error("Project not found");

    Article article;
    article.project = *project;
    article.title = title;

    return articles.save(article);
});
```

If the callback returns normally, NovaBoot commits. If it throws, NovaBoot rolls
back according to the transaction options.
