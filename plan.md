# NovaBoot Data Layer — Spring Data JPA / Hibernate Equivalent

A full-stack data manager for NovaBoot with:
- **PostgreSQL** via ODB (ORM + connection pool)  
- **Redis / Valkey cluster** via redis++ (cache-aside, read-through, cluster-aware)  
- **Application-wide TOML configuration** (replaces magic string constructor args)  
- **MongoDB placeholder** — architecture is pre-wired but no implementation yet

---

## Architecture Overview

```
┌─────────────────────────────────────────────┐
│  UserController / UserService               │  ← existing, unchanged
└──────────────────┬──────────────────────────┘
                   │  injects
┌──────────────────▼──────────────────────────┐
│  UserRepository                             │  ← user-defined, annotated
│  [[=data::sql_repository{}]]               │  ← or [[=data::cache_repository{}]]
│  inherits CrudRepository<User, int>         │
└───────────┬──────────────────┬──────────────┘
            │                  │
 ┌──────────▼──────┐  ┌────────▼────────────┐
 │  SqlDataSource  │  │  RedisDataSource     │
 │  (ODB + pool)   │  │  (redis++ cluster)   │
 └─────────────────┘  └──────────────────────┘
```

---

## Annotation Surface (Developer-Facing API)

### Entities (mapping to DB)
```cpp
[[=data::entity{"users"}]]           // maps to table "users"
struct User {
    [[=data::id{}]]                   // PRIMARY KEY
    int id;

    [[=data::column{"full_name"}]]    // custom column name
    std::string name;

    std::string email;                // column = field name by default

    [[=data::cacheable{.ttl_seconds = 60}]] // cache in Redis by PK
    // ^ on entity struct = cache whole entity; on field = partial cache
};
```

### Repositories
```cpp
// SQL CRUD (ODB-backed)
[[=novaboot::di::repository{}]]
struct UserRepository : data::CrudRepository<User, int> {
    // Generated: find_by_id, find_all, save, delete_by_id, exists_by_id, count
    // Custom: add any ODB query methods here
    std::vector<User> find_by_email(std::string_view email);
};

// Redis cache-only repository
[[=novaboot::di::repository{}]]
struct UserCacheRepository : data::CacheRepository<User, int> {
    // Generated: get, put, evict, exists — key = "User:<id>"
};

// Combined: cache-aside with SQL fallback (READ-THROUGH / WRITE-BEHIND)
[[=novaboot::di::repository{}]]
struct UserSmartRepository : data::CachingCrudRepository<User, int> {
    // on miss: falls through to SQL, writes back to Redis with configured TTL
};
```

### TOML Application Config (`resources/application.toml`)
```toml
[server]
host = "0.0.0.0"
port = 4433
workers = 0              # 0 = hardware_concurrency

[datasource.postgres]
host     = "localhost"
port     = 5432
user     = "nova"
password = "secret"
database = "novadb"
pool.min = 2
pool.max = 20

[datasource.redis]
mode     = "cluster"             # "single" | "cluster" | "sentinel"
nodes    = ["127.0.0.1:6379"]
password = ""
pool.size = 8
pool.timeout_ms = 2000
read_from = "replica_preferred"  # "master" | "replica_preferred" | "master_preferred"
cluster.slot_refresh_interval_ms = 10000
```

### Server Builder (extended)
```cpp
auto cfg = novaboot::config::AppConfig::load("resources/application.toml");

auto app = Server::create()
    .config(cfg)                                        // ← NEW
    .data_source<data::PgsqlDataSource>(cfg.postgres()) // ← NEW
    .data_source<data::RedisDataSource>(cfg.redis())    // ← NEW
    .workers(cfg.server().workers())
    .bind(cfg.server().host(), cfg.server().port())
    .tls("cert.pem", "key.pem")
    .di_container(di_root)
    .build();
```

---

## Proposed Changes

### 1 — Dependencies

#### [MODIFY] [Dependencies.cmake](file:///home/uday/Projects/novaboot/cmake/Dependencies.cmake)
- Install and find `tomlplusplus` (`pacman -S tomlplusplus`)
- Find `PkgConfig::REDIS_PP` (`redis++`)
- Find `PkgConfig::LIBODB` and `PkgConfig::LIBODB_PGSQL`
- Guard all three under `NOVABOOT_ENABLE_DATA` CMake option (ON by default)

#### [MODIFY] [CMakeLists.txt](file:///home/uday/Projects/novaboot/CMakeLists.txt)
- Add option `NOVABOOT_ENABLE_DATA ON`
- Conditionally compile `src/data/**` sources
- Add `odb_compile()` macro wrapper (custom command invoking `odb` tool on tagged entity headers)
- Link `libodb`, `libodb-pgsql`, `libpq`, `redis++`, `hiredis` to `novaboot` target when data is enabled

---

### 2 — Configuration System (`novaboot/config`)

> [!IMPORTANT]
> App-wide config — not just data. Lives in `resources/application.toml`.

#### [NEW] [config/app_config.h](file:///home/uday/Projects/novaboot/include/novaboot/config/app_config.h)
```
novaboot::config::AppConfig         // top-level loaded config
novaboot::config::ServerConfig      // [server] section
novaboot::config::PostgresConfig    // [datasource.postgres] section
novaboot::config::RedisConfig       // [datasource.redis] section
novaboot::config::RedisMode         // enum: Single, Cluster, Sentinel
novaboot::config::ReadFrom          // enum: Master, ReplicaPreferred, MasterPreferred
```
- `AppConfig::load(path)` — loads and parses the TOML file with toml++
- `AppConfig::get<T>(key)` — generic typed getter for arbitrary sections (for user-defined config)
- The full `AppConfig` is registered as a DI singleton bean (injectable by any service)

#### [NEW] [config/app_config.cpp](file:///home/uday/Projects/novaboot/src/config/app_config.cpp)
- Implements TOML parsing with `toml::parse_file()`
- Maps TOML tables → `PostgresConfig`, `RedisConfig`, `ServerConfig` structs

---

### 3 — Data Annotations (`novaboot/data`)

#### [NEW] [data/data_attributes.h](file:///home/uday/Projects/novaboot/include/novaboot/data/data_attributes.h)
All structural annotation types (no `std::string_view`, all `char[]`):

| Annotation | Target | Description |
|---|---|---|
| `[[=data::entity{"table"}]]` | struct | Maps to an ODB-persisted table |
| `[[=data::id{}]]` | field | Marks the primary key |
| `[[=data::column{"col"}]]` | field | Overrides the column name |
| `[[=data::cacheable{.ttl_seconds=60}]]` | struct or field | Marks entity/field for Redis caching |
| `[[=data::transient{}]]` | field | Not persisted (ODB `#pragma db transient`) |
| `[[=data::version{}]]` | field | ODB optimistic lock version column |
| `[[=data::sql_repository{}]]` | struct | Wires a `CrudRepository` to a `SqlDataSource` |
| `[[=data::cache_repository{}]]` | struct | Wires a `CacheRepository` to a `RedisDataSource` |

- Extend `novaboot::di::detail::is_managed_component()` to also recognize `data::sql_repository` and `data::cache_repository` as managed beans

#### [MODIFY] [di/attributes.h](file:///home/uday/Projects/novaboot/include/novaboot/di/attributes.h)
- Add `data::sql_repository` and `data::cache_repository` to the `is_managed_component` consteval check so the DI scanner picks them up automatically

---

### 4 — Repository Interfaces (`novaboot/data`)

#### [NEW] [data/crud_repository.h](file:///home/uday/Projects/novaboot/include/novaboot/data/crud_repository.h)
Abstract CRTP base class:
```cpp
template<typename Entity, typename Id>
struct CrudRepository {
    virtual std::optional<Entity> find_by_id(const Id& id) = 0;
    virtual std::vector<Entity>   find_all()                = 0;
    virtual Entity                save(const Entity& e)     = 0;
    virtual void                  delete_by_id(const Id& id)= 0;
    virtual bool                  exists_by_id(const Id& id)= 0;
    virtual std::size_t           count()                   = 0;
    virtual ~CrudRepository() = default;
};
```

#### [NEW] [data/cache_repository.h](file:///home/uday/Projects/novaboot/include/novaboot/data/cache_repository.h)
Redis-backed key-value store interface:
```cpp
template<typename Entity, typename Id>
struct CacheRepository {
    virtual std::optional<Entity> get(const Id& id)        = 0;
    virtual void                  put(const Id& id,
                                      const Entity& e,
                                      std::chrono::seconds ttl) = 0;
    virtual void                  evict(const Id& id)      = 0;
    virtual bool                  exists(const Id& id)     = 0;
    virtual ~CacheRepository() = default;
};
```

#### [NEW] [data/caching_crud_repository.h](file:///home/uday/Projects/novaboot/include/novaboot/data/caching_crud_repository.h)
Composes `CrudRepository` + `CacheRepository` with automatic cache-aside:
```cpp
template<typename Entity, typename Id>
struct CachingCrudRepository : CrudRepository<Entity, Id> {
    // find_by_id: check cache → miss → SQL → write back to cache
    // save:       SQL first  → evict cache (write-through)
    // delete:     SQL first  → evict cache
    // find_all / count: always SQL (no caching of list queries)
};
```

---

### 5 — PostgreSQL Data Source (`novaboot/data/pgsql`)

#### [NEW] [data/pgsql/pgsql_data_source.h](file:///home/uday/Projects/novaboot/include/novaboot/data/pgsql/pgsql_data_source.h)
```cpp
class PgsqlDataSource {
public:
    explicit PgsqlDataSource(const config::PostgresConfig& cfg);

    // Get or lease a pooled ODB connection
    odb::pgsql::database& db();

    // Execute in a transaction (RAII)
    template<typename F>
    auto transact(F&& fn) -> decltype(fn(db()));

private:
    std::unique_ptr<odb::pgsql::database> db_;
    // Uses odb::pgsql::connection_pool_factory internally
};
```

#### [NEW] [data/pgsql/pgsql_data_source.cpp](file:///home/uday/Projects/novaboot/src/data/pgsql/pgsql_data_source.cpp)
- Constructs `odb::pgsql::database` with `connection_pool_factory(max, min)`
- `transact(F)` wraps call in `odb::transaction`

#### [NEW] [data/pgsql/pgsql_repository_base.h](file:///home/uday/Projects/novaboot/include/novaboot/data/pgsql/pgsql_repository_base.h)
Implements `CrudRepository<E, Id>` over an ODB database:
```cpp
template<typename Entity, typename Id>
class PgsqlRepositoryBase : public CrudRepository<Entity, Id> {
public:
    explicit PgsqlRepositoryBase(PgsqlDataSource& ds);

    std::optional<Entity> find_by_id(const Id& id) override;
    std::vector<Entity>   find_all() override;
    Entity                save(const Entity& e) override;
    void                  delete_by_id(const Id& id) override;
    bool                  exists_by_id(const Id& id) override;
    std::size_t           count() override;
protected:
    PgsqlDataSource& ds_;
};
```
- Uses `odb::query<Entity>()` for find_all, count
- Uses `db.persist(e)` / `db.update(e)` / `db.erase<Entity>(id)` for write

---

### 6 — Redis Data Source (`novaboot/data/redis`)

#### [NEW] [data/redis/redis_data_source.h](file:///home/uday/Projects/novaboot/include/novaboot/data/redis/redis_data_source.h)
```cpp
class RedisDataSource {
public:
    explicit RedisDataSource(const config::RedisConfig& cfg);

    // Returns a reference to the underlying client (single or cluster)
    // Use for raw operations; prefer typed CacheRepository for entities
    sw::redis::Redis&        client();         // single-node / sentinel
    sw::redis::RedisCluster& cluster_client(); // cluster mode
    bool                     is_cluster() const noexcept;

private:
    config::RedisMode mode_;
    std::unique_ptr<sw::redis::Redis>        redis_;
    std::unique_ptr<sw::redis::RedisCluster> cluster_;
};
```

#### [NEW] [data/redis/redis_data_source.cpp](file:///home/uday/Projects/novaboot/src/data/redis/redis_data_source.cpp)
Cluster construction:
```cpp
sw::redis::ConnectionOptions opts;
opts.host = cfg.nodes[0].host;
opts.port = cfg.nodes[0].port;
opts.password = cfg.password;

sw::redis::ConnectionPoolOptions pool_opts;
pool_opts.size = cfg.pool.size;
pool_opts.wait_timeout = std::chrono::milliseconds(cfg.pool.timeout_ms);

sw::redis::ClusterOptions cluster_opts;
cluster_opts.slot_map_refresh_interval =
    std::chrono::milliseconds(cfg.cluster.slot_refresh_interval_ms);

// ReadFrom is mapped to sw::redis::Role:
// ReplicaPreferred → Role::SLAVE (prefers replica, falls back to master)
// Master           → Role::MASTER
// MasterPreferred  → Role::MASTER (with user-land fallback via try/catch)
auto role = to_redis_role(cfg.read_from);

cluster_ = std::make_unique<sw::redis::RedisCluster>(opts, pool_opts, role, cluster_opts);
```

#### [NEW] [data/redis/redis_repository_base.h](file:///home/uday/Projects/novaboot/include/novaboot/data/redis/redis_repository_base.h)
Implements `CacheRepository<E, Id>`:
```cpp
template<typename Entity, typename Id>
class RedisRepositoryBase : public CacheRepository<Entity, Id> {
public:
    explicit RedisRepositoryBase(RedisDataSource& ds,
                                 std::string key_prefix,
                                 std::chrono::seconds default_ttl);

    std::optional<Entity> get(const Id& id) override;
    void put(const Id& id, const Entity& e, std::chrono::seconds ttl) override;
    void evict(const Id& id) override;
    bool exists(const Id& id) override;

private:
    std::string make_key(const Id& id) const;
    RedisDataSource& ds_;
    std::string      key_prefix_;    // e.g. "User"
    std::chrono::seconds default_ttl_;
};
```
- Serialization: reuses `novaboot::json::serialize` / `deserialize` for JSON in Redis
- Key format: `"<Prefix>:<id>"` (e.g. `"User:42"`)
- Uses `SET key val EX ttl` via redis++ pipeline for atomic set-with-TTL

---

### 7 — ODB Code Generation Integration

#### [MODIFY] [cmake/OdbGenerate.cmake](file:///home/uday/Projects/novaboot/cmake/OdbGenerate.cmake) — [NEW]
```cmake
function(odb_generate TARGET ENTITY_HEADER)
  get_filename_component(STEM ${ENTITY_HEADER} NAME_WE)
  set(ODB_OUT_CXX "${CMAKE_CURRENT_BINARY_DIR}/odb/${STEM}.odb.cxx")
  set(ODB_OUT_HXX "${CMAKE_CURRENT_BINARY_DIR}/odb/${STEM}-odb.hxx")

  add_custom_command(
    OUTPUT  ${ODB_OUT_CXX} ${ODB_OUT_HXX}
    COMMAND odb
        --database pgsql
        --std c++17              # ODB compiler targets C++17 for pragmas
        --generate-query
        --generate-schema
        --schema-format sql
        --output-dir ${CMAKE_CURRENT_BINARY_DIR}/odb
        ${ENTITY_HEADER}
    DEPENDS ${ENTITY_HEADER}
    COMMENT "Generating ODB mapping for ${ENTITY_HEADER}"
  )
  target_sources(${TARGET} PRIVATE ${ODB_OUT_CXX})
  target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/odb)
endfunction()
```

> [!NOTE]
> ODB uses C++17-level header pragmas (`#pragma db object`), but the entity struct itself is compiled as C++26. The trick is a thin `*-odb.hxx` adapter that ODB emits — the user's application code never includes it directly; `PgsqlRepositoryBase` includes it internally. This keeps user code reflection-only.

#### Entity annotation → ODB pragma bridge
A small header `data/odb_pragma_bridge.h` is included inside `pgsql_repository_base.h`:
- Uses `static_assert` + `#pragma db` driven by a pre-generation script (part of `cmake/OdbGenerate.cmake`)
- The `odb_generate()` function reads `[[=data::entity]]` / `[[=data::id]]` / `[[=data::column]]` annotations from the entity header and emits a temporary `.odb.h` with the appropriate `#pragma db` directives, then feeds that to the ODB compiler

> [!IMPORTANT]
> **This is the most complex part**: we need a bridge between C++26 annotations and ODB's C++17 `#pragma db` system. The strategy is a **CMake-time codegen step** (a small Python/CMake script) that reads the annotations as text and emits the `#pragma db` pragmas into a shadow header. ODB then processes the shadow header. Users never write `#pragma db` at all.

---

### 8 — Caching + Reconnection

#### [NEW] [data/redis/redis_reconnect_policy.h](file:///home/uday/Projects/novaboot/include/novaboot/data/redis/redis_reconnect_policy.h)
- redis++ `ConnectionOptions::connect_timeout` and `socket_timeout` handle initial failure
- On `sw::redis::Error` (network drop): catch in `RedisRepositoryBase::get/put`, retry up to N times with exponential backoff, then rethrow as `data::CacheUnavailableException`
- Fallback: `CachingCrudRepository` catches `CacheUnavailableException` and goes directly to SQL without failing the request

```
RedisDataSource::cluster_ → connection pool manages physical reconnects
   ↓
RedisRepositoryBase      → catches sw::redis::Error, retries up to 3×
   ↓
CachingCrudRepository    → catches CacheUnavailableException → falls through to SQL
   ↓ (emits warning log, still serves the request from DB)
```

---

### 9 — DI Wiring

#### [MODIFY] [data/data_module.h](file:///home/uday/Projects/novaboot/include/novaboot/data/data_module.h) — [NEW]
```cpp
[[=novaboot::di::module_tag{}]]
struct DataModule {
    [[=novaboot::di::bean{}]]
    PgsqlDataSource make_pgsql(AppConfig& cfg) {
        return PgsqlDataSource(cfg.postgres());
    }

    [[=novaboot::di::bean{}]]
    RedisDataSource make_redis(AppConfig& cfg) {
        return RedisDataSource(cfg.redis());
    }

    [[=novaboot::di::bean{}]]
    AppConfig make_config() {
        return AppConfig::load("resources/application.toml");
    }
};
```
- `PgsqlDataSource`, `RedisDataSource`, `AppConfig` are all registered as DI singletons
- Any `UserRepository : CachingCrudRepository<User, int>` injects `PgsqlDataSource&` + `RedisDataSource&` via constructor

---

### 10 — Server Builder Extension

#### [MODIFY] [core/server.h](file:///home/uday/Projects/novaboot/include/novaboot/core/server.h)
Add to `Builder`:
```cpp
Builder& config(const config::AppConfig& cfg);
Builder& data_source(std::shared_ptr<data::PgsqlDataSource> ds);
Builder& data_source(std::shared_ptr<data::RedisDataSource> ds);
```
- Both store the data sources and register them into the DI `RootContainer` before `build()` runs
- `config()` stores the `AppConfig` and also registers it as a DI singleton bean

#### [MODIFY] [src/core/server.cpp](file:///home/uday/Projects/novaboot/src/core/server.cpp)
- Implement the three new builder methods

---

### 11 — Exceptions

#### [NEW] [data/exceptions.h](file:///home/uday/Projects/novaboot/include/novaboot/data/exceptions.h)
```cpp
namespace novaboot::data {
    struct DataException         : std::runtime_error { using runtime_error::runtime_error; };
    struct EntityNotFoundException : DataException    { ... };
    struct CacheUnavailableException : DataException  { ... };
    struct DataSourceException   : DataException      { ... };
    struct OptimisticLockException : DataException    { ... };
}
```
- `EntityNotFoundException` is thrown by `find_by_id` when `std::nullopt` would be returned but the caller is using the throwing overload (`find_by_id_or_throw`)
- `CacheUnavailableException` is thrown by Redis layer after retry exhaustion

---

### 12 — Example: Updated `UserRepository` and `UserService`

#### [MODIFY] [examples/src/repository/user_repository.h](file:///home/uday/Projects/novaboot/examples/src/repository/user_repository.h)
Replace the in-memory stub with:
```cpp
[[=novaboot::di::repository{}]]
struct UserRepository : data::CachingCrudRepository<examples::model::User, int> {
    explicit UserRepository(data::PgsqlDataSource& db,
                            data::RedisDataSource& cache)
        : data::CachingCrudRepository<examples::model::User, int>(db, cache, "User", 60s) {}

    // Custom query — uses ODB query<User>()
    std::vector<examples::model::User> find_by_email(std::string_view email);
};
```

#### [MODIFY] [examples/src/model/user.h](file:///home/uday/Projects/novaboot/examples/src/model/user.h)
Add data annotations:
```cpp
[[=lombok::data{}]] [[=lombok::builder{}]]
[[=data::entity{"users"}]]
struct User {
    [[=data::id{}]]
    int id;

    [[=data::column{"full_name"}]] [[=novaboot::validation::size{.min=2,.max=20}]]
    std::string name;

    [[=novaboot::validation::email{}]]
    std::string email;

    std::string role;

    [[=data::cacheable{.ttl_seconds=120}]]  // cache whole entity for 2 min

    #include "user.lombok.h"
};
```

#### [NEW] [examples/src/resources/application.toml](file:///home/uday/Projects/novaboot/examples/src/resources/application.toml)
Complete config file for the sample app

#### [MODIFY] [examples/src/main.cpp](file:///home/uday/Projects/novaboot/examples/src/main.cpp)
```cpp
auto cfg = novaboot::config::AppConfig::load("resources/application.toml");

auto app = Server::create()
    .config(cfg)
    .data_source<data::PgsqlDataSource>(cfg.postgres())
    .data_source<data::RedisDataSource>(cfg.redis())
    .workers(cfg.server().workers())
    .bind(cfg.server().host(), cfg.server().port())
    .tls("cert.pem", "key.pem")
    .di_container(di_root)
    .build();
```

---

### 13 — Umbrella Header

#### [MODIFY] [novaboot.h](file:///home/uday/Projects/novaboot/include/novaboot/novaboot.h)
Add when `NOVABOOT_ENABLE_DATA` is on:
```cpp
// Data layer (PostgreSQL + Redis)
#include "novaboot/config/app_config.h"
#include "novaboot/data/data_attributes.h"
#include "novaboot/data/crud_repository.h"
#include "novaboot/data/cache_repository.h"
#include "novaboot/data/caching_crud_repository.h"
#include "novaboot/data/exceptions.h"
#include "novaboot/data/pgsql/pgsql_data_source.h"
#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "novaboot/data/redis/redis_data_source.h"
#include "novaboot/data/redis/redis_repository_base.h"
#include "novaboot/data/data_module.h"
```

---

## File Listing Summary

| Status | File | Purpose |
|---|---|---|
| `[NEW]` | `cmake/OdbGenerate.cmake` | ODB codegen CMake macro |
| `[NEW]` | `include/novaboot/config/app_config.h` | TOML AppConfig |
| `[NEW]` | `src/config/app_config.cpp` | TOML loader impl |
| `[NEW]` | `include/novaboot/data/data_attributes.h` | All data annotations |
| `[NEW]` | `include/novaboot/data/crud_repository.h` | CrudRepository<E,Id> |
| `[NEW]` | `include/novaboot/data/cache_repository.h` | CacheRepository<E,Id> |
| `[NEW]` | `include/novaboot/data/caching_crud_repository.h` | Combined cache-aside |
| `[NEW]` | `include/novaboot/data/exceptions.h` | Data layer exceptions |
| `[NEW]` | `include/novaboot/data/data_module.h` | DI module wiring beans |
| `[NEW]` | `include/novaboot/data/pgsql/pgsql_data_source.h` | ODB PG source |
| `[NEW]` | `src/data/pgsql/pgsql_data_source.cpp` | ODB PG source impl |
| `[NEW]` | `include/novaboot/data/pgsql/pgsql_repository_base.h` | ODB CRUD impl |
| `[NEW]` | `include/novaboot/data/redis/redis_data_source.h` | redis++ cluster source |
| `[NEW]` | `src/data/redis/redis_data_source.cpp` | redis++ source impl |
| `[NEW]` | `include/novaboot/data/redis/redis_repository_base.h` | Redis cache impl |
| `[MODIFY]` | `cmake/Dependencies.cmake` | Add toml++, redis++, ODB |
| `[MODIFY]` | `CMakeLists.txt` | NOVABOOT_ENABLE_DATA option |
| `[MODIFY]` | `include/novaboot/core/server.h` | Builder `.config()` + `.data_source()` |
| `[MODIFY]` | `src/core/server.cpp` | Implement builder methods |
| `[MODIFY]` | `include/novaboot/di/attributes.h` | Extend is_managed_component |
| `[MODIFY]` | `include/novaboot/novaboot.h` | Re-export data headers |
| `[MODIFY]` | `examples/src/model/user.h` | Add data annotations to User |
| `[MODIFY]` | `examples/src/repository/user_repository.h` | Replace stub with real repo |
| `[MODIFY]` | `examples/src/main.cpp` | Use config + data_source builder |
| `[NEW]` | `examples/src/resources/application.toml` | Sample app config |

---

## Open Questions

> [!NOTE]
> **MongoDB placeholder** — Architecture is pre-wired (the `DataSource` concept is abstracted behind a type-erased interface). When MongoDB support is added, we'll add `MongoDataSource` implementing the same interface without touching existing code.

> [!IMPORTANT]
> **ODB Pragma Bridge Complexity** — The ODB compiler is a C++ source-to-source tool that requires `#pragma db` directives. Since our entities use C++26 reflection annotations instead, we need a small CMake-time code generator that scans the entity header text (not compiled), extracts annotation lines (e.g. `[[=data::entity{"users"}]]`), and emits a shadow header with `#pragma db object table("users")` etc. This shadow header is what ODB processes. **This is the most involved engineering task in this plan.**

> [!NOTE]
> **Transaction Propagation** — Currently each repository method opens its own ODB transaction. Full Spring-style `@Transactional` propagation (sharing one transaction across multiple repository calls in a service method) is NOT in scope for this iteration — this would require a thread-local or coroutine-local transaction context. Marked as `// TODO: transaction propagation` in the code.

---

## Verification Plan

### Automated Tests
```
ctest --output-on-failure           # all existing 18 tests must still pass
ctest -R test_data_*                # new data layer unit tests
```

New tests:
- `test_app_config` — load TOML file, assert typed field access
- `test_pgsql_repository` — integration test against a local PostgreSQL (Docker)
- `test_redis_repository` — unit test with redis-mock or local Redis
- `test_caching_crud` — verify cache-miss → SQL fallback → cache write-back

### Manual Verification
- Start `sample_app` with a live PostgreSQL + Redis cluster
- `GET /api/users/1` → first hit: SQL fetch + Redis write; second hit: Redis cache hit (check Redis `EXISTS User:1`)
- Kill a Redis node → request still succeeds from PostgreSQL (graceful degradation)
