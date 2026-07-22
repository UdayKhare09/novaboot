# NovaBoot Data/JPA 1.0 compatibility contract

NovaBoot provides a typed, reflection-driven persistence layer inspired by the
useful application-facing parts of JPA and Spring Data JPA. It is not a Java
JPA provider and does not emulate Hibernate bytecode enhancement.

## Supported contract

- Entity mapping: `Entity`, `Table`, `Id`, generated ids, columns, transient
  fields, LOB/JSON/temporal fields, string or ordinal enums, lifecycle hooks,
  and version columns.
- Repositories: CRUD, typed predicates, grouping, projections, counts,
  existence checks, paging, sorting, fetch hints, and collection helpers.
- Relationships: owning `ManyToOne`, inverse `OneToMany`, owner-side
  `ManyToMany` with explicit join tables, save/remove cascades, orphan removal,
  and join-row cleanup. See [the cascade contract](orm-cascade-contract.md).
- Concurrency: `Version` increments on update and stale saves throw
  `OptimisticLockException`.
- Transactions: explicit `TransactionManager` callbacks and
  `Transactional` metadata with propagation, isolation, read-only, timeout,
  rollback-for, and no-rollback-for settings.
- Migrations: explicit, versioned migrations with an application-owned ledger.
  See [migrations](migrations.md).
- Lazy relations: explicit `Lazy<T>` and `LazyCollection<T>` holders, not
  hidden proxies. See [relationship modelling](relationship-modeling.md).

## Deliberate differences from Hibernate and Spring Data JPA

- There is no persistence context, automatic dirty checking, `EntityManager`,
  JPQL, Criteria API, or runtime method-name query derivation. Save changes
  explicitly with a repository; use typed repository/query-builder methods for
  queries.
- There are no Java-style transparent lazy proxies. A relation that may load
  later is visibly a `Lazy<T>` or `LazyCollection<T>`, so application code owns
  its lifetime and transaction boundary.
- Schema changes are never inferred and applied automatically. Run migrations
  during bootstrap, then use schema validation; a mismatch remains a startup
  failure until an explicit migration resolves it.
- `save_all` is a lifecycle-preserving collection operation, not an implicit
  database protocol batch: it keeps per-entity generated-id, cascade, orphan,
  version, and callback ordering intact. For uniform SQL, use
  `Connection::execute_batch(sql, parameter_sets)`. SQLite prepares once and
  rebinds each parameter set; PostgreSQL prepares once then sends executions
  through libpq pipeline mode, avoiding a round trip per row. A batch does not
  start a transaction, so callers choose its all-or-nothing boundary.
- Repository writes are eager. `CrudRepository::flush()` is consequently an
  explicit compatibility boundary, not a hidden dirty-check flush; it is a
  no-op for the synchronous SQLite and PostgreSQL drivers and supports future
  buffered drivers without changing application code.

## Batch benchmark

The opt-in `bench_database_batch` target compares one-by-one SQLite inserts
with the same 100 rows through `execute_batch`. It excludes table cleanup from
timing and reports throughput in rows, so it is useful for detecting a
prepared-statement reuse regression without making a timing threshold part of
the deterministic test suite.

```sh
cmake -S . -B build-bench -DNOVABOOT_BUILD_BENCH=ON
cmake --build build-bench --target bench_database_batch -j2
./build-bench/bench/bench_database_batch --benchmark_min_time=1s
```
- Local SQLite and PostgreSQL datasource pools bound acquisition time (30
  seconds by default), emit a warning when a connection lease exceeds its
  configurable threshold (60 seconds by default), and safely close returned
  leases after datasource shutdown. Use driver connection/query timeouts as
  well, and close the datasource during shutdown.

## Operational baseline

Use a PostgreSQL `connect_timeout` in the connection string, close the
`DataSource` during application shutdown, expose `db::health_contributor` for
readiness, and optionally wrap the datasource in `ObservingDataSource` for
timing metrics. Never log SQL parameter values as a diagnostic substitute.

The behavior in this document is covered by repository, schema, migration,
transaction, batch, and PostgreSQL integration tests. Features outside the
contract above should be treated as unsupported until they are documented and
tested.
