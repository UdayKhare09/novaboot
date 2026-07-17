# NovaBoot migrations

NovaBoot migrations are explicit application code. The framework can validate
or create tables from entity metadata, but it does not auto-diff entities into
schema changes. That keeps production schema changes intentional and reviewable.

## Running migrations

Run migrations during bootstrap, before schema validation or repository use:

```cpp
#include "novaboot/db/migration.h"

auto datasource = di_root.resolve<std::shared_ptr<novaboot::db::DataSource>>();

novaboot::db::MigrationRunner::run(*datasource, {
    novaboot::db::Migration::sql(
        1,
        "create audit events",
        "CREATE TABLE IF NOT EXISTS audit_events ("
        "id BIGSERIAL PRIMARY KEY, "
        "message TEXT NOT NULL, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)"
    ),
    novaboot::db::Migration::callback(
        2,
        "seed required lookup rows",
        "lookup-seed-v1",
        [](novaboot::db::Connection& connection) {
            connection.execute(
                "INSERT INTO audit_events(message) VALUES (?)",
                {novaboot::db::Parameter("application installed")});
        }
    ),
});
```

`Migration::sql` derives its checksum from the SQL statement. For callback
migrations, provide a stable checksum string and update it only when the
callback is intentionally changed.

## Ledger and safety checks

Migrations are recorded in `novaboot_schema_migrations` with:

- `version`
- `description`
- `checksum`
- `success`
- `installed_at`

NovaBoot fails fast when:

- the database contains a failed migration row;
- an applied migration is removed from the application migration list;
- an applied migration keeps the same version but changes description;
- an applied migration keeps the same version but changes checksum.

If a migration fails, NovaBoot records it as dirty and rethrows the original
failure. Fix the database and repair the ledger before starting the app again.

## Recommended bootstrap shape

Use this order for application startup:

```cpp
try {
    novaboot::db::MigrationRunner::run(*datasource, migrations());

    novaboot::db::SchemaGenerator::validate_table<User>(*datasource);
    novaboot::db::SchemaGenerator::validate_table<Order>(*datasource);
} catch (const std::exception& error) {
    spdlog::critical("Database bootstrap failed: {}", error.what());
    return EXIT_FAILURE;
}
```

For local demos, `create_table<T>()` is convenient. For production apps, prefer
migrations plus `validate_table<T>()`: migrations perform the change, validation
guards against drift.

## What migrations are not

NovaBoot migrations intentionally do not perform Hibernate-style automatic
schema updates. If the entity model and existing database schema differ, the
app should fail at startup; add an explicit migration to move the database to
the new shape.
