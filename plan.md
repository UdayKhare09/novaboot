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
- [x] Document the cascade contract clearly, especially that many-to-many
  delete cleans join rows but does not delete shared related entities.
- [x] Fix enum query parameter binding so predicates against string-backed enum
  columns bind the reflected enum name instead of the numeric ordinal.

## Milestone 6 — framework usability and safe lazy loading

- [x] Improve request body DTO binding for nested JSON objects and vectors.
- [x] Add explicit DI constructor selection with `@Autowired`.
- [x] Add an in-process app integration test harness.
- [x] Add explicit `novaboot::db::Lazy<T>` support for safe lazy
  `@ManyToOne` loading on first access.
- [x] Extend lazy loading to collection relations with explicit
  `novaboot::db::LazyCollection<T>` for `@OneToMany` and `@ManyToMany`.
- [x] Add explicit `query().fetch<&Entity::relation>()` overrides. To keep
  parent pagination stable and avoid row explosion, collection fetches use
  secondary selects on the active query connection instead of SQL join-fetches.
- [x] Add true batch collection loading across multiple parents, so lazy
  collection hydration can collapse N secondary selects into grouped `IN`
  queries.
- [x] Add SQL join-fetch support for to-one relations where it is safe and
  useful, while keeping collection fetches secondary-select by default.
- [x] Define optional session/transaction-bound lazy loading semantics; the
  current default deliberately reacquires connections on access so detached
  entities do not pin a pool connection.

## Milestone 7 — Spring-like service ergonomics

- [x] Add Spring-like `@Transactional` metadata and a usable
  `TransactionManager` / transaction callback API for service-layer code.
- [x] Support transaction propagation, timeout, and rollback/no-rollback
  exception rules in `TransactionManager`.
- [x] Carry read-only and isolation metadata/options; enforce them where the
  driver can do so safely without leaking connection state.
- [x] Add generated dispatch-level invocation support for controller methods
  annotated with `@Transactional`.
- [x] Add reflection-backed service method invocation for methods annotated
  with `@Transactional` via `TransactionManager::invoke<&Service::method>()`.
- [ ] Add generated/proxy-style service wrapping for methods annotated with
  `@Transactional`, so direct service-to-service calls can be intercepted
  without explicitly using `TransactionManager::invoke`.
- [x] Add typed repository shortcuts such as `find_one_by<&T::field>()`,
  `exists_by<&T::field>()`, and `delete_by<&T::field>()`.
- [x] Add relation-scoped query predicates for filtering across safe to-one
  relationships.
- [x] Add relation-scoped sorting across safe to-one relationships.
- [ ] Upgrade Knowledge Hub to use `LazyCollection` and `fetch` in its read
  paths.
- [x] Upgrade Knowledge Hub write paths to use transaction helpers and
  `@Transactional` metadata.

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
- **2026-07-17 — usability fixes and lazy loading:** documented the cascade
  contract in `docs/orm-cascade-contract.md`; fixed string-backed enum query
  predicates to bind reflected enum names; allowed POST body DTOs to contain
  nested objects and vectors without query-binding compile failures; added
  `@Autowired` constructor selection; added `novaboot::testing::AppTestClient`
  for in-process route tests; and introduced explicit `novaboot::db::Lazy<T>`
  for safe lazy `@ManyToOne` loading on first access. Full suite passes,
  **37/37**, including live Docker PostgreSQL. Knowledge Hub was smoke-tested
  with a real HTTPS POST using nested `metadata` and vector `contributor_ids`.
- **2026-07-17 — lazy collections and fetch overrides:** added
  `novaboot::db::LazyCollection<T>` for lazy `@OneToMany` and `@ManyToMany`
  relations, count-without-hydration support, and save semantics that leave
  untouched lazy collections alone instead of treating them as empty. Added
  `query().fetch<&Entity::relation>()`; to-one fetches hydrate during row
  mapping, while collection fetches use secondary selects on the active query
  connection. Regression coverage verifies lazy one-to-many and many-to-many
  first-access loading plus explicit fetch hydration.
- **2026-07-17 — batched collection fetches:** collection fetch overrides now
  batch-hydrate all returned parents with grouped `IN` queries for inverse
  one-to-many and join-table many-to-many relations, then assign loaded
  vectors/lazy collections back to each parent. Added regression coverage for
  fetching multiple parents with loaded lazy collections.
- **2026-07-17 — to-one join fetch and transaction-bound lazy mode:**
  `query().fetch<&Entity::many_to_one>()` now emits a parent subquery plus
  `LEFT JOIN` for the fetched target, mapping aliased target columns directly
  into loaded `Lazy<T>` / entity fields without secondary selects. Default lazy
  loading remains detached-safe and reacquires a connection on access; when a
  repository/query is constructed with an explicit transaction connection,
  lazy loaders retain and reuse that pinned connection. Regression coverage
  verifies loaded lazy to-one fetches and same-transaction lazy access.
- **2026-07-17 — transaction service ergonomics:** added `@Transactional`
  method metadata and `novaboot::db::TransactionManager`, a Spring
  `TransactionTemplate`-style helper that executes callbacks with a pinned
  connection, commits on success, and rolls back on exceptions. Added service
  regression coverage proving commit/rollback behaviour and annotation
  reflection.
- **2026-07-17 — Spring-like transaction knobs:** expanded
  `@Transactional` with propagation, isolation, read-only, and timeout
  metadata. Expanded `TransactionManager` with `REQUIRED`, `REQUIRES_NEW`,
  `SUPPORTS`, `NOT_SUPPORTED`, `MANDATORY`, and `NEVER` propagation,
  rollback-only marking for nested scopes, timeout rollback, and typed
  `rollback_for<T>()` / `no_rollback_for<T>()` rules. PostgreSQL transactions
  receive best-effort `SET TRANSACTION` isolation/read-only statements; SQLite
  keeps those options as metadata to avoid leaking connection-level PRAGMAs
  through the pool. Regression coverage verifies propagation, rollback rules,
  timeout rollback, and annotation metadata.
- **2026-07-17 — ambient and route-level transactions:** repositories now
  join the current thread-bound transaction automatically when they are not
  already scoped to an explicit connection. Generated route handlers wrap
  controller methods annotated with `@Transactional` in `TransactionManager`,
  so controller-level transaction boundaries roll back normal injected
  repository work. Regression coverage verifies ambient repository rollback
  and route-dispatch rollback on exception.
- **2026-07-17 — Knowledge Hub transaction upgrade:** Knowledge Hub now
  registers a `TransactionManager` bean and its write paths use transaction
  helper scopes with normal injected repositories joining the ambient
  transaction. `create_article` and `seed_demo` are annotated with
  `@Transactional` metadata for the future service-proxy layer. The
  `knowledge_hub_app` target builds successfully.
- **2026-07-17 — transactional service invocation:** added
  `TransactionManager::invoke<&Service::method>(service, args...)`, which uses
  reflection to read a service method's `@Transactional` metadata and execute
  it through the same propagation/rollback/timeout engine used by route
  dispatch. Unannotated methods are invoked plainly. Regression coverage proves
  annotated service methods commit/roll back ambient repository work while raw
  direct C++ calls remain non-interceptable without an explicit generated proxy
  type.
- **2026-07-17 — typed repository shortcuts:** added field-pointer repository
  helpers `find_one_by`, `find_all_by`, `exists_by`, `count_by`, and
  `delete_by`. These keep the C++ API type-safe without Java-style method-name
  parsing. Regression coverage verifies lookup, existence, counting, and
  deletion.
- **2026-07-17 — relation-scoped predicates:** added typed
  `where_related`, `and_related`, and `or_related` query predicates for
  filtering an entity by fields on a safe `@ManyToOne` target, e.g.
  filtering articles by `author.name`. The implementation uses a stable
  subquery predicate to avoid root-row duplication and preserve pagination.
- **2026-07-17 — relation-scoped sorting:** added typed
  `order_by_related<&Entity::many_to_one, &Target::field>()` for safe to-one
  relationships. It orders by a scalar target-field subquery, avoiding
  duplicate root rows and staying compatible with existing pagination.
