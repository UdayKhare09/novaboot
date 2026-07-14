#include <gtest/gtest.h>
#include "novaboot/db/repository.h"
#include "novaboot/db/exceptions.h"
#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/annotations/stereotypes.h"
#include <string>
#include <chrono>

using namespace novaboot::db;
using namespace novaboot::annotations;

// ============================================================================
// Helper — in-memory SQLite DataSource
// ============================================================================
static std::shared_ptr<DataSource> make_sqlite() {
    return std::make_shared<novaboot::db::sqlite::SqliteDataSource>(":memory:", 1);
}

// ============================================================================
// 1. @Transient  — field skipped in SELECT and INSERT
// ============================================================================

struct [[= Entity("transient_items") ]] TransientItem {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string label;

    [[= Transient() ]]
    std::string cached_value; // never persisted
};

struct TransientItemRepo : CrudRepository<TransientItem, int> {
    explicit TransientItemRepo(std::shared_ptr<DataSource> ds)
        : CrudRepository<TransientItem, int>(ds) {}
};

TEST(TransientAnnotationTest, SkippedInSelectAndInsert) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute("CREATE TABLE transient_items (id INTEGER PRIMARY KEY AUTOINCREMENT, label TEXT);");
    }

    TransientItemRepo repo(ds);
    TransientItem item{};
    item.label = "hello";
    item.cached_value = "should_not_persist";

    auto saved = repo.save(item);
    EXPECT_GT(saved.id, 0);

    // The SELECT has no 'cached_value' column, so map_row_to_entity must not
    // try to read it from the result set — if it does, col mapping breaks and
    // label would be wrong.
    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->label, "hello");
    EXPECT_TRUE(loaded->cached_value.empty()); // never written to DB
}

// ============================================================================
// 2. @Enumerated String
// ============================================================================

enum class Status { Active, Inactive, Pending };

struct [[= Entity("enum_items") ]] EnumItem {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= Enumerated(EnumType::String) ]]
    Status status = Status::Pending;
};

struct EnumItemRepo : CrudRepository<EnumItem, int> {
    explicit EnumItemRepo(std::shared_ptr<DataSource> ds)
        : CrudRepository<EnumItem, int>(ds) {}
};

TEST(EnumeratedStringTest, SaveAndReload) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute("CREATE TABLE enum_items (id INTEGER PRIMARY KEY AUTOINCREMENT, status TEXT);");
    }

    EnumItemRepo repo(ds);
    EnumItem item{};
    item.status = Status::Active;

    auto saved = repo.save(item);
    EXPECT_GT(saved.id, 0);

    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->status, Status::Active);
}

// ============================================================================
// 3. @Enumerated Ordinal
// ============================================================================

struct [[= Entity("ordinal_items") ]] OrdinalItem {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= Enumerated(EnumType::Ordinal) ]]
    Status level = Status::Pending;
};

struct OrdinalItemRepo : CrudRepository<OrdinalItem, int> {
    explicit OrdinalItemRepo(std::shared_ptr<DataSource> ds)
        : CrudRepository<OrdinalItem, int>(ds) {}
};

TEST(EnumeratedOrdinalTest, SaveAndReload) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute("CREATE TABLE ordinal_items (id INTEGER PRIMARY KEY AUTOINCREMENT, level INTEGER);");
    }

    OrdinalItemRepo repo(ds);
    OrdinalItem item{};
    item.level = Status::Inactive; // ordinal 1

    auto saved = repo.save(item);
    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->level, Status::Inactive);
}

// ============================================================================
// 4. @Version — optimistic locking
// ============================================================================

struct [[= Entity("versioned_items") ]] VersionedItem {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string title;

    [[= Version() ]]
    int version = 0;
};

struct VersionedItemRepo : CrudRepository<VersionedItem, int> {
    explicit VersionedItemRepo(std::shared_ptr<DataSource> ds)
        : CrudRepository<VersionedItem, int>(ds) {}
};

TEST(VersionOptimisticLockTest, IncrementOnUpdate) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute(R"(CREATE TABLE versioned_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            version INTEGER
        );)");
    }

    VersionedItemRepo repo(ds);
    VersionedItem item{};
    item.title = "v1";

    auto saved = repo.save(item);
    EXPECT_EQ(saved.version, 1); // inserted with version=1

    saved.title = "v2";
    auto updated = repo.save(saved);
    EXPECT_EQ(updated.version, 2); // incremented

    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->version, 2);
    EXPECT_EQ(loaded->title, "v2");
}

TEST(VersionOptimisticLockTest, ConflictThrows) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute(R"(CREATE TABLE versioned_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            version INTEGER
        );)");
    }

    VersionedItemRepo repo(ds);
    VersionedItem item{};
    item.title = "original";

    auto saved = repo.save(item);  // version=1 in DB

    // Simulate two concurrent readers getting the same version
    auto copy_a = saved; // version=1
    auto copy_b = saved; // version=1

    // copy_a saves successfully → DB version becomes 2
    copy_a.title = "updated by A";
    repo.save(copy_a);

    // copy_b tries to update with stale version=1 → conflict
    copy_b.title = "updated by B";
    EXPECT_THROW(repo.save(copy_b), OptimisticLockException);
}

// ============================================================================
// 5. @Table with schema-qualified name
// ============================================================================

struct [[= Entity() ]] [[= Table("tbl_products") ]] TableItem {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string name;
};

struct TableItemRepo : CrudRepository<TableItem, int> {
    explicit TableItemRepo(std::shared_ptr<DataSource> ds)
        : CrudRepository<TableItem, int>(ds) {}
};

TEST(TableAnnotationTest, NameOverride) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute("CREATE TABLE tbl_products (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT);");
    }

    TableItemRepo repo(ds);
    TableItem item{};
    item.name = "widget";

    auto saved = repo.save(item);
    EXPECT_GT(saved.id, 0);

    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name, "widget");
}

// ============================================================================
// 6. @Column(insertable=false) and @Column(updatable=false)
// ============================================================================

struct [[= Entity("col_flag_items") ]] ColFlagItem {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string title;

    [[= Column("created_at", true, false, false) ]]
    std::string created_at; // non-insertable and non-updatable

    [[= Column("score", true, true, false) ]]
    int score = 99; // insertable but non-updatable
};

struct ColFlagItemRepo : CrudRepository<ColFlagItem, int> {
    explicit ColFlagItemRepo(std::shared_ptr<DataSource> ds)
        : CrudRepository<ColFlagItem, int>(ds) {}
};

TEST(ColumnFlagsTest, InsertableAndUpdatable) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute(R"(CREATE TABLE col_flag_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            created_at TEXT DEFAULT 'default_time',
            score INTEGER
        );)");
    }

    ColFlagItemRepo repo(ds);
    ColFlagItem item{};
    item.title = "test_item";
    item.created_at = "ignored_time"; // should be ignored on insert
    item.score = 50; // should be saved on insert

    auto saved = repo.save(item);
    
    // Verify insertable=false: it was not sent to DB, so DB default was used
    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->title, "test_item");
    EXPECT_EQ(loaded->created_at, "default_time"); // DB default instead of "ignored_time"
    EXPECT_EQ(loaded->score, 50);

    // Verify updatable=false:
    loaded->title = "updated_title";
    loaded->created_at = "new_ignored_time";
    loaded->score = 100; // should be ignored on update

    auto updated = repo.save(*loaded);
    
    auto reloaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->title, "updated_title");
    EXPECT_EQ(reloaded->created_at, "default_time"); // unchanged
    EXPECT_EQ(reloaded->score, 50); // unchanged because score is updatable=false
}

// ============================================================================
// 7. Lifecycle hooks: @PrePersist / @PostPersist / @PreUpdate / @PostUpdate / @PostLoad
// ============================================================================

struct [[= Entity("lifecycle_items") ]] LifecycleItem {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string label;
    int pre_persist_calls  = 0;
    int post_persist_calls = 0;
    int pre_update_calls   = 0;
    int post_update_calls  = 0;
    int post_load_calls    = 0;

    [[= PrePersist() ]]
    void on_pre_persist()  { pre_persist_calls++;  }

    [[= PostPersist() ]]
    void on_post_persist() { post_persist_calls++; }

    [[= PreUpdate() ]]
    void on_pre_update()   { pre_update_calls++;   }

    [[= PostUpdate() ]]
    void on_post_update()  { post_update_calls++;  }

    [[= PostLoad() ]]
    void on_post_load()    { post_load_calls++;    }
};

struct LifecycleItemRepo : CrudRepository<LifecycleItem, int> {
    explicit LifecycleItemRepo(std::shared_ptr<DataSource> ds)
        : CrudRepository<LifecycleItem, int>(ds) {}
};

TEST(LifecycleHookTest, PrePostPersist) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute(R"(CREATE TABLE lifecycle_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            label TEXT,
            pre_persist_calls INTEGER,
            post_persist_calls INTEGER,
            pre_update_calls INTEGER,
            post_update_calls INTEGER,
            post_load_calls INTEGER
        );)");
    }

    LifecycleItemRepo repo(ds);
    LifecycleItem item{};
    item.label = "test";

    auto saved = repo.save(item); // INSERT
    EXPECT_EQ(saved.pre_persist_calls,  1);
    EXPECT_EQ(saved.post_persist_calls, 1);
    EXPECT_EQ(saved.pre_update_calls,   0);
    EXPECT_EQ(saved.post_update_calls,  0);
}

TEST(LifecycleHookTest, PrePostUpdate) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute(R"(CREATE TABLE lifecycle_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            label TEXT,
            pre_persist_calls INTEGER,
            post_persist_calls INTEGER,
            pre_update_calls INTEGER,
            post_update_calls INTEGER,
            post_load_calls INTEGER
        );)");
    }

    LifecycleItemRepo repo(ds);
    LifecycleItem item{};
    item.label = "initial";

    auto saved = repo.save(item); // INSERT
    saved.label = "modified";
    auto updated = repo.save(saved); // UPDATE

    // counters accumulate across lifecycle calls
    EXPECT_EQ(updated.pre_update_calls,  1);
    EXPECT_EQ(updated.post_update_calls, 1);
}

TEST(LifecycleHookTest, PostLoad) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute(R"(CREATE TABLE lifecycle_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            label TEXT,
            pre_persist_calls INTEGER,
            post_persist_calls INTEGER,
            pre_update_calls INTEGER,
            post_update_calls INTEGER,
            post_load_calls INTEGER
        );)");
    }

    LifecycleItemRepo repo(ds);
    LifecycleItem item{};
    item.label = "load test";
    auto saved = repo.save(item);

    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->post_load_calls, 1); // called once after SELECT
}

// ============================================================================
// 8. Todo App Model Verification
// ============================================================================
#include "../../examples/todo_notes/src/model/todo.h"

struct TodoTestRepository : CrudRepository<todo_notes::model::Todo, std::string> {
    explicit TodoTestRepository(std::shared_ptr<DataSource> ds)
        : CrudRepository<todo_notes::model::Todo, std::string>(ds) {}
};

TEST(TodoAppModelTest, AnnotationsIntegrity) {
    auto ds = make_sqlite();
    {
        auto conn = ds->get_connection();
        conn->execute(R"(CREATE TABLE todos (
            id TEXT PRIMARY KEY,
            user_id TEXT NOT NULL,
            title TEXT NOT NULL,
            description TEXT,
            completed BOOLEAN NOT NULL,
            version INTEGER,
            priority TEXT
        );)");
    }

    TodoTestRepository repo(ds);
    
    todo_notes::model::Todo todo{};
    todo.user_id = "user_123";
    todo.title = "Buy milk";
    todo.description = "Whole milk";
    todo.completed = false;
    todo.priority = todo_notes::model::TodoPriority::High;
    todo.temp_note = "Do not save me to database";

    // 1. INSERT verification
    auto saved = repo.save(todo);
    EXPECT_FALSE(saved.id.empty());
    EXPECT_EQ(saved.version, 1); // @Version starts at 1
    EXPECT_EQ(saved.priority, todo_notes::model::TodoPriority::High);

    // 2. Load verification
    auto loaded = repo.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->version, 1);
    EXPECT_EQ(loaded->priority, todo_notes::model::TodoPriority::High);
    EXPECT_TRUE(loaded->temp_note.empty()); // @Transient not loaded (empty)

    // 3. UPDATE verification
    loaded->title = "Buy soy milk";
    auto updated = repo.save(*loaded);
    EXPECT_EQ(updated.version, 2); // @Version incremented

    // 4. Stale update verification (Optimistic Lock)
    todo_notes::model::Todo stale_copy = *loaded; // version 1
    stale_copy.title = "Conflict write";
    EXPECT_THROW(repo.save(stale_copy), OptimisticLockException);
}

