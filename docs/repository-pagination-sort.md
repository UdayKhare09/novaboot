# Repository pagination and sorting

NovaBoot repositories expose Spring Data-style paging, but keep the C++ API
typed and explicit.

## Basic page request

```cpp
using novaboot::db::Pageable;

auto page = article_repository.find_page(
    Pageable::of(0, 20)
        .sort_by<Article, &Article::published_at>(false)
        .sort_by<Article, &Article::title>());

for (const auto& article : page.content) {
    // render article
}

if (page.has_next()) {
    // expose next-page link
}
```

Page numbers are zero-based. `Pageable` requires a non-negative page number and
a positive page size.

## Page metadata

`Page<T>` contains:

- `content`
- `page`
- `size`
- `total_elements`
- `total_pages()`
- `has_next()`
- `has_previous()`
- `is_first()`
- `is_last()`
- `number_of_elements()`

This mirrors the useful parts of Spring Data `Page<T>` without tying the API to
web concepts.

## Typed sort helpers

Use typed sort helpers when the sort column is known at compile time:

```cpp
using novaboot::db::sort_by;
using novaboot::db::sort_desc;

auto title_asc = sort_by<Article, &Article::title>();
auto newest_first = sort_desc<Article, &Article::published_at>();

auto page = article_repository.find_page(
    Pageable::of(0, 10)
        .sorted(newest_first)
        .sorted(title_asc));
```

The helper resolves the reflected column name, so renaming a C++ field or
changing its `[[= Column(...) ]]` mapping is caught closer to compile time.

## QueryBuilder paging

Use `query().page(pageable)` when a page needs predicates or fetch hints:

```cpp
auto page = article_repository.query()
    .where<&Article::status>(novaboot::db::Op::Equal, ArticleStatus::Published)
    .fetch<&Article::contributors>()
    .page(Pageable::of(0, 12).sort_by<Article, &Article::title>());
```

The count query uses the same predicates. The list query applies the requested
sort, limit, offset, and fetch strategy.

## Dynamic UI grids

For admin grids or user-selected sort columns, raw-column `Sort` remains
available:

```cpp
novaboot::db::Pageable pageable = novaboot::db::Pageable::of(page, size);
pageable.sorted(novaboot::db::Sort{.column = "title", .ascending = true});
```

Validate user-provided column names before constructing a raw `Sort`.
