# NovaBoot Data / JPA Roadmap

## Goal

Evolve the existing reflection-based repository layer into a reliable,
explicitly-scoped Spring Data JPA-inspired API.  The goal is practical C++
data access—not a byte-for-byte recreation of the Java JPA runtime.

## Current baseline

- [x] Entity metadata, `CrudRepository`, typed query building, SQLite and
  PostgreSQL drivers.
- [x] UUIDv7 IDs, enums, transient fields, optimistic locking, column write
  flags, and entity lifecycle callbacks.
- [x] Focused SQLite persistence tests pass. PostgreSQL tests are present but
  skip when the local PostgreSQL service is unavailable.

## Milestone 1 — repository correctness and core ergonomics

- [x] Replace positional `SELECT *` entity mapping with explicit persisted
  column lists and result-column-name lookup.
- [x] Add the remaining small repository primitives: `count`, `save_all`,
  `find_all_by_id`, and delete-by-entity semantics.
- [x] Add query predicates with correct SQL semantics: `IS NULL`, `IN`,
  `BETWEEN`, and grouped `AND` / `OR` expressions.
- [x] Add `Page<T>` / `Pageable` execution, including a total-count query.
- [x] Add regression tests for null predicates, pagination, and the remaining
  repository primitives.
- [x] Add regression coverage for reordered result columns and custom column
  names.

## Milestone 2 — transaction boundary

- [x] Provide an RAII transaction scope which borrows one pooled connection,
  commits on success, and rolls back on exceptions.
- [x] Allow repositories and queries to participate in that scope.
- [x] Test optimistic-lock failures inside transactions.

## Milestone 3 — schema support

- [x] Generate dialect-aware `CREATE TABLE` DDL from entity and column
  metadata.
- [x] Honour nullability, uniqueness, length/type hints, IDs, and versions.
- [x] Define a deliberately small migration/versioning API rather than trying
  to infer unsafe schema alterations automatically.
- [x] Validate existing SQLite/PostgreSQL tables at bootstrap and stop the
  application on any structural mismatch.

## Milestone 4 — associations

- [x] Implement `@ManyToOne` and `@JoinColumn` first, with explicit eager
  loading semantics for direct entity values. Schema foreign-key generation,
  repository foreign-key persistence, follow-up loading, and cycle protection
  are covered; collection associations remain separate.
- [x] Treat collection relationships as non-column fields and support eager
  inverse `@OneToMany(mapped_by)` hydration through the child-side
  `@ManyToOne/@JoinColumn`.
- [x] Implement explicit collection persistence/cascade behaviour for direct
  values: inverse `@OneToMany` saves children after assigning the mapped parent,
  and owner-side `@ManyToMany` rewrites join-table rows.
- [x] Generate, validate, persist, and eagerly hydrate explicit
  `@ManyToMany + @JoinTable(name, join_column, inverse_join_column)` mappings.
- [x] Defer lazy proxies until a C++ lifetime-safe design is agreed.

## Milestone 5 — polish, annotation depth, and app ergonomics

- [x] Add relation deletion semantics: join-row cleanup on delete and a
  deliberate orphan-removal policy for inverse collections.
- [x] Map the deferred JPA-like annotations: `@Temporal`, `@Lob`, and `@Json`.
- [x] Add richer repository ergonomics around derived queries, projections, and
  relation-aware sorting/filtering.
- [x] Write a compact guide/example showing schema generation, migrations,
  transactions, and associations together.
- [ ] Document the cascade contract clearly, especially that many-to-many
  delete cleans join rows but does not delete shared related entities.
- [ ] Fix enum query parameter binding so predicates against string-backed enum
  columns bind the reflected enum name instead of the numeric ordinal.

## Deferred JPA-like annotations

`@Temporal`, `@Lob`, and `@Json` are mapped. `@Temporal` controls date/time
DDL and storage formatting, `@Lob` maps large strings/blobs to text/blob
storage, and `@Json` stores raw JSON strings or reflected C++ values as
SQLite `TEXT` / PostgreSQL `JSONB`.

## Checkpoints

- **2026-07-17 — baseline:** audited the repository layer; all 35 configured
  tests pass. The two PostgreSQL integration tests skip because PostgreSQL is
  not running locally.
- **2026-07-17 — Milestone 1 / mapping:** repository reads now select explicit
  persisted columns and map by result-column name. Added a regression test for
  a custom-column entity returned in a different result order.
- **2026-07-17 — Milestone 1 / repository primitives:** added and tested
  `count`, `save_all`, `find_all_by_id`, and `delete_entity` against SQLite.
- **2026-07-17 — verification:** the full 35-test suite passes when run with
  `io_uring` permission. PostgreSQL integration cases still skip because no
  PostgreSQL server is running locally.
- **2026-07-17 — Milestone 1 / queries:** added `IS NULL`, `IN`, `BETWEEN`,
  grouped predicates, safe pageable sorting, and `Page<T>` total counts. The
  page implementation releases its count-query connection before fetching
  content, so it is safe with a one-connection pool. Persistence regression
  tests pass; the old file-backed SQLite fixture was made self-contained.
- **2026-07-17 — PostgreSQL check:** PostgreSQL tests were retried with host
  network access, but `localhost:5432` refused the connection and no local
  socket or Docker port mapping was present. The tests remain conditional.
- **2026-07-17 — Milestone 2 / transactions:** added header-only RAII
  `Transaction`, which pins one `Connection` and rolls back unless committed.
  `CrudRepository` and `QueryBuilder` can share that connection; SQLite tests
  verify commit and automatic rollback with a one-connection pool.
- **2026-07-17 — verification after query and transaction work:** full suite
  passes, **35/35** tests. PostgreSQL cases are still skipped because the
  configured TCP listener is unavailable.
- **2026-07-17 — PostgreSQL integration:** Docker PostgreSQL is available at
  `localhost:5432`; existing CRUD and portable-type integration tests pass.
  Added a two-transaction PostgreSQL regression test that verifies a stale
  `@Version` update raises `OptimisticLockException`.
- **2026-07-17 — Milestone 3 / schema generation:** added `SchemaGenerator`
  for portable SQLite/PostgreSQL `CREATE TABLE IF NOT EXISTS` DDL. It maps
  scalar, UUID, timestamp, blob, and enum fields; honours IDs, generation,
  nullability, uniqueness, lengths, raw column definitions, versions, and
  transient fields. SQLite and live PostgreSQL DDL tests pass.
- **2026-07-17 — schema guard:** `SchemaGenerator` now validates existing
  tables instead of silently accepting `CREATE TABLE IF NOT EXISTS`. The Todo
  app logs a `SchemaMismatchException`, shuts down DI, and exits before server
  startup when a table differs. No migrations or automatic alterations occur.
- **2026-07-17 — verification with Docker PostgreSQL:** full suite passes,
  **36/36** tests. PostgreSQL CRUD, portable-type, generated-DDL, and
  two-transaction optimistic-lock coverage all executed rather than skipped.
- **2026-07-17 — explicit migrations:** added `MigrationRunner` and `Migration`
  for ordered, transactional, application-defined schema changes. Applied
  versions are recorded in `novaboot_schema_migrations`; duplicate, malformed,
  or database-only versions fail fast. No entity-diff migration is attempted.
- **2026-07-17 — associations foundation:** chose direct entity values for
  `@ManyToOne`, requiring explicit `@JoinColumn`. Generated DDL now includes
  typed foreign keys; repository writes the related `@Id` and restores an
  identity stub on load. Eager hydration is deliberately deferred until its
  cycle and query-loading policy is defined.
- **2026-07-17 — verification after association foundation:** full suite
  passes, **36/36** tests, including live Docker PostgreSQL integration.
- **2026-07-17 — eager ManyToOne:** follow-up relation queries now reuse the
  active connection (including one-connection pools and transactions), restore
  the related entity, and stop recursive cycles with an in-flight identity set.
  The focused schema, CRUD, and annotation tests pass.
- **2026-07-17 — inverse OneToMany:** collection relationships are now skipped
  by column selection, schema generation, inserts, and updates. Eager
  `@OneToMany(mapped_by)` loads child rows through the child foreign key while
  keeping recursive parent references as identity stubs. Full suite passes,
  **36/36**, including live Docker PostgreSQL integration.
- **2026-07-17 — Milestone 4 complete:** inverse `@OneToMany` now cascades
  direct child saves by assigning the mapped parent reference first.
  Owner-side `@ManyToMany` now requires explicit `@JoinTable` metadata, creates
  and validates the join table, cascades related saves, rewrites owner join
  rows, and eagerly hydrates related rows. Lazy proxies remain deliberately
  deferred. Full suite passes, **36/36**, including live Docker PostgreSQL.
- **2026-07-17 — cascade/delete policy:** save cascades now honour
  `CascadeType::Persist`, `Merge`, and `All`; inverse `@OneToMany` supports
  opt-in orphan removal and recursive child deletes for `Remove`/`All`.
  Owner-side `@ManyToMany` deletes clean join rows but deliberately keep shared
  related entities. Full suite passes, **36/36**, including live Docker
  PostgreSQL.
- **2026-07-17 — annotation mapping:** `@Temporal(Date/Time/Timestamp)` now
  drives dialect-aware DDL and storage formatting; `@Json` stores raw JSON
  strings or reflected C++ values as SQLite `TEXT` / PostgreSQL `JSONB`; `@Lob`
  is covered for large text/blob schema mapping. Added SQLite persistence
  regression coverage. Full suite passes, **36/36**, including live Docker
  PostgreSQL.
- **2026-07-17 — query ergonomics:** `QueryBuilder` now supports typed
  projections, filtered `count()` / `exists()`, and relation predicates that
  bind a related entity's `@Id` for `@ManyToOne` fields. Pageable totals reuse
  the filtered count path. Full suite passes, **36/36**, including live Docker
  PostgreSQL.
- **2026-07-17 — Knowledge Hub example app:** added
  `examples/knowledge_hub`, a Postgres-backed UI example that exercises
  configuration property injection, schema validation, explicit migrations,
  transactions, repositories, projections, `@Json`, `@Temporal`,
  eager `@ManyToOne`, inverse `@OneToMany`, and owner `@ManyToMany` join
  tables. Built target `knowledge_hub_app`, booted it against Docker
  PostgreSQL, verified `/` serves the static UI, `POST /api/seed` returns
  seeded dashboard data with correct stats, and `GET /api/articles/:id`
  hydrates JSON metadata plus many-to-many contributors.
