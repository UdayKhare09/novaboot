#pragma once
#include "novaboot/di/scope.h"

namespace novaboot::annotations {

// ---------------------------------------------------------------------------
// DI / Component stereotypes
// ---------------------------------------------------------------------------

/// Mark a struct/class as a generic component managed by DI container.
struct Component {
    di::Scope scope = di::Scope::Singleton;
    consteval Component() = default;
    consteval explicit Component(di::Scope s) : scope(s) {}
};

/// Mark a struct/class as a business logic service component managed by DI container.
struct Service {
    di::Scope scope = di::Scope::Singleton;
    consteval Service() = default;
    consteval explicit Service(di::Scope s) : scope(s) {}
};

/// Mark a struct/class as a data access repository component managed by DI container.
struct Repository {
    di::Scope scope = di::Scope::Singleton;
    consteval Repository() = default;
    consteval explicit Repository(di::Scope s) : scope(s) {}
};

/// Mark a struct/class as a REST Controller mapping HTTP routes.
struct RestController {
    char base_path[64] = {};
    consteval RestController() = default;
    consteval explicit RestController(const char* path) {
        int i = 0;
        while (path[i] && i < 63) {
            base_path[i] = path[i];
            i++;
        }
        base_path[i] = '\0';
    }
};

/// Mark a struct/class as a global controller advice for handling exceptions.
struct ControllerAdvice {
    consteval ControllerAdvice() = default;
};

/// Mark a controller advice method as an exception handler.
struct ExceptionHandler {
    consteval ExceptionHandler() = default;
};

/// Mark a class/struct as a configuration class containing factory methods.
struct Configuration {
    consteval Configuration() = default;
};

/// Mark a configuration class method as a bean factory method.
struct Bean {
    di::Scope scope = di::Scope::Singleton;
    consteval Bean() = default;
    consteval explicit Bean(di::Scope s) : scope(s) {}
};

/// Specify execution order for components like filters/middlewares.
struct Order {
    int value = 0;
    consteval Order() = default;
    consteval Order(int val) : value(val) {}
};

// ---------------------------------------------------------------------------
// ORM — Core Entity Definition
// ---------------------------------------------------------------------------

/// Mark a struct/class as a database entity.
/// The optional name overrides the default snake_case+plural table name.
struct Entity {
    char name[64] = {};
    consteval Entity() = default;
    consteval Entity(const char* n) {
        int i = 0;
        while (n[i] && i < 63) {
            name[i] = n[i];
            i++;
        }
        name[i] = '\0';
    }
};

/// Supplemental table configuration: schema, table name (takes priority over @Entity name),
/// and future unique-constraint metadata. Apply alongside @Entity.
struct Table {
    char name[64]   = {};   // explicit table name override
    char schema[64] = {};   // database schema (e.g. "public")

    consteval Table() = default;
    consteval Table(const char* n, const char* s = "") {
        int i = 0;
        while (n[i] && i < 63) { name[i]   = n[i];   i++; } name[i]   = '\0';
        i = 0;
        while (s[i] && i < 63) { schema[i] = s[i]; i++; } schema[i] = '\0';
    }
};

// ---------------------------------------------------------------------------
// ORM — Primary Key
// ---------------------------------------------------------------------------

/// Designate the primary key field.
struct Id {
    consteval Id() = default;
};

enum class GenerationType {
    AutoIncrement,   ///< Database identity / serial column
    UUID             ///< Driver-generated UUIDv7 string
};

/// Configure primary key generation strategy.
struct GeneratedValue {
    GenerationType strategy = GenerationType::AutoIncrement;
    consteval GeneratedValue() = default;
    consteval GeneratedValue(GenerationType s) : strategy(s) {}
};

// ---------------------------------------------------------------------------
// ORM — Attribute Mapping
// ---------------------------------------------------------------------------

/// Customise the column mapping for a field.
struct Column {
    char name[64]              = {};      ///< Explicit column name (default: field identifier)
    char column_definition[128] = {};     ///< Raw SQL type, e.g. "VARCHAR(200) NOT NULL"
    bool nullable   = true;               ///< Allow NULL values
    bool unique     = false;              ///< Add UNIQUE constraint
    bool insertable = true;               ///< Include in INSERT statement
    bool updatable  = true;               ///< Include in UPDATE SET clause
    int  length     = 255;                ///< Length hint for string columns

    consteval Column() = default;

    /// Minimal constructor: column name only.
    consteval explicit Column(const char* col_name) {
        int i = 0;
        while (col_name[i] && i < 63) { name[i] = col_name[i]; i++; }
        name[i] = '\0';
    }

    /// Constructor with name + nullable.
    consteval Column(const char* col_name, bool is_nullable)
        : nullable(is_nullable)
    {
        int i = 0;
        while (col_name[i] && i < 63) { name[i] = col_name[i]; i++; }
        name[i] = '\0';
    }

    /// Constructor with name, nullable, insertable, updatable.
    consteval Column(const char* col_name, bool is_nullable, bool is_insertable, bool is_updatable)
        : nullable(is_nullable), insertable(is_insertable), updatable(is_updatable)
    {
        int i = 0;
        while (col_name[i] && i < 63) { name[i] = col_name[i]; i++; }
        name[i] = '\0';
    }
};

/// Mark a field as transient — excluded from all SQL (SELECT, INSERT, UPDATE).
struct Transient {
    consteval Transient() = default;
};

/// Controls how an enum field is persisted.
enum class EnumType { Ordinal, String };
struct Enumerated {
    EnumType value = EnumType::String;
    consteval Enumerated() = default;
    consteval explicit Enumerated(EnumType t) : value(t) {}
};

/// Hint the SQL date/time precision for a chrono field.
enum class TemporalType { Date, Time, Timestamp };
struct Temporal {
    TemporalType value = TemporalType::Timestamp;
    consteval Temporal() = default;
    consteval explicit Temporal(TemporalType t) : value(t) {}
};

/// Mark a field as a Large Object (BLOB for vector<uint8_t>, CLOB for string).
struct Lob {
    consteval Lob() = default;
};

/// Mark an integer field as the optimistic-locking version counter.
/// The ORM auto-initialises it to 1 on INSERT and increments it on every UPDATE.
/// A concurrent-modification conflict throws novaboot::db::OptimisticLockException.
struct Version {
    consteval Version() = default;
};

/// Mark string field to store formatted JSON objects.
struct Json {
    consteval Json() = default;
};

// ---------------------------------------------------------------------------
// ORM — Lifecycle Callbacks
// Applied to void member functions of the entity class.
// ---------------------------------------------------------------------------

/// Called before INSERT (new entity).
struct PrePersist  { consteval PrePersist()  = default; };
/// Called after  INSERT (new entity, ID is populated).
struct PostPersist { consteval PostPersist() = default; };
/// Called before UPDATE (existing entity).
struct PreUpdate   { consteval PreUpdate()   = default; };
/// Called after  UPDATE (existing entity).
struct PostUpdate  { consteval PostUpdate()  = default; };
/// Called after  any SELECT that returns this entity.
struct PostLoad    { consteval PostLoad()    = default; };

// ---------------------------------------------------------------------------
// ORM — Relationships (Phase 2 — annotations defined, not yet wired into ORM)
// ---------------------------------------------------------------------------

enum class FetchType   { Lazy, Eager };
enum class CascadeType { Persist, Merge, Remove, All };

struct ManyToOne {
    FetchType   fetch   = FetchType::Eager;
    CascadeType cascade = CascadeType::All;
    bool optional = true;
    consteval ManyToOne() = default;
};

struct OneToMany {
    FetchType   fetch   = FetchType::Lazy;
    CascadeType cascade = CascadeType::All;
    char mapped_by[64] = {};
    consteval OneToMany() = default;
    consteval explicit OneToMany(const char* mb) {
        int i = 0;
        while (mb[i] && i < 63) { mapped_by[i] = mb[i]; i++; }
        mapped_by[i] = '\0';
    }
};

struct OneToOne {
    FetchType   fetch   = FetchType::Eager;
    CascadeType cascade = CascadeType::All;
    bool optional = true;
    consteval OneToOne() = default;
};

struct ManyToMany {
    FetchType   fetch   = FetchType::Lazy;
    CascadeType cascade = CascadeType::All;
    consteval ManyToMany() = default;
};

struct JoinColumn {
    char name[64]              = {};
    char referenced_column[64] = {};
    bool nullable = true;
    bool unique   = false;
    consteval JoinColumn() = default;
    consteval explicit JoinColumn(const char* col_name) {
        int i = 0;
        while (col_name[i] && i < 63) { name[i] = col_name[i]; i++; }
        name[i] = '\0';
    }
};

struct JoinTable {
    char name[64]        = {};
    char join_column[64] = {};
    char inverse_join_column[64] = {};
    consteval JoinTable() = default;
    consteval explicit JoinTable(const char* n) {
        int i = 0;
        while (n[i] && i < 63) { name[i] = n[i]; i++; }
        name[i] = '\0';
    }
};

} // namespace novaboot::annotations
