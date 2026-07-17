# NovaBoot ORM cascade contract

NovaBoot intentionally keeps cascade behaviour explicit and conservative.

## Save cascades

- `CascadeType::Persist` cascades saves from a new owner to related values.
- `CascadeType::Merge` cascades saves from an existing owner to related values.
- `CascadeType::All` enables both persist and merge save cascades, plus remove
  cascades where supported.
- Collection save cascades currently apply to direct value collections:
  inverse `@OneToMany(mapped_by)` and owner `@ManyToMany`.

## `@OneToMany`

`@OneToMany` is inverse-side only and must name the owner field with
`mapped_by`.

- The child table owns the foreign key through its `@ManyToOne/@JoinColumn`.
- Saving a parent with a save cascade assigns the parent reference into each
  child before saving the child.
- `orphan_removal=true` deletes child rows that were previously attached to
  the parent but are no longer present in the parent collection.
- Delete cascades for `Remove`/`All` recursively delete child entities.

## `@ManyToMany`

`@ManyToMany` requires an explicit `@JoinTable`.

- The owner side rewrites join-table rows on save.
- Deleting an owner always cleans its join-table rows.
- Deleting an owner does **not** delete shared related entities. This is
  deliberate: a many-to-many target may be referenced by other owners.
- Save cascades may save related entities before join rows are written, but
  remove cascades stop at the join table.

## `@ManyToOne`

`@ManyToOne` owns a foreign-key column through `@JoinColumn`.

- Direct entity fields are persisted by writing the related entity id.
- `FetchType::Eager` hydrates the related entity immediately.
- `FetchType::Lazy` on a direct entity field leaves an identity stub.
- Use `novaboot::db::Lazy<T>` for real lazy loading on first access.

## Lazy relations

Lazy loading is explicit in C++:

```cpp
[[= ManyToOne(FetchType::Lazy) ]]
[[= JoinColumn("author_id") ]]
novaboot::db::Lazy<Author> author;
```

Calling `author.get()`, `author->field`, or `*author` performs the deferred
load once and then returns the cached entity.
