#include <gtest/gtest.h>

#include "novaboot/annotations/stereotypes.h"
#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/db/migration.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/schema.h"

#include <chrono>
#include <ctime>
#include <string>
#include <vector>

using namespace novaboot::annotations;
using namespace novaboot::db;
using namespace novaboot::db::sqlite;

enum class SchemaStatus { Pending, Active };

struct [[= Entity("schema_test_entities") ]] SchemaTestEntity {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= Column("email", false, true, true, true, 120) ]]
    std::string email;

    bool active = false;
    double balance = 0;
    Uuid external_id;
    std::chrono::system_clock::time_point created_at;
    std::vector<std::uint8_t> payload;

    [[= Enumerated(EnumType::String) ]]
    SchemaStatus status = SchemaStatus::Pending;

    [[= Transient() ]]
    std::string cache_only;
};

struct [[= Entity("schema_authors") ]] SchemaAuthor {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;
    std::string name;
};

struct [[= Entity("schema_articles") ]] SchemaArticle {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= ManyToOne() ]]
    [[= JoinColumn("author_id") ]]
    SchemaAuthor author;
};

struct [[= Entity("schema_blog_posts") ]] SchemaBlogPost;

struct [[= Entity("schema_blogs") ]] SchemaBlog {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string title;

    [[= OneToMany("blog", FetchType::Eager, CascadeType::All, true) ]]
    std::vector<SchemaBlogPost> posts;
};

struct [[= Entity("schema_blog_posts") ]] SchemaBlogPost {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string title;

    [[= ManyToOne() ]]
    [[= JoinColumn("blog_id") ]]
    SchemaBlog blog;
};

struct [[= Entity("schema_tags") ]] SchemaTag {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string label;
};

struct [[= Entity("schema_tagged_posts") ]] SchemaTaggedPost {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    std::string title;

    [[= ManyToMany(FetchType::Eager) ]]
    [[= JoinTable("schema_post_tags", "post_id", "tag_id") ]]
    std::vector<SchemaTag> tags;
};

struct SchemaJsonSettings {
    bool email = false;
    int level = 0;
};

struct [[= Entity("schema_annotation_entities") ]] SchemaAnnotationEntity {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::AutoIncrement) ]]
    int id = 0;

    [[= Temporal(TemporalType::Date) ]]
    std::chrono::system_clock::time_point event_date;

    [[= Temporal(TemporalType::Time) ]]
    std::chrono::system_clock::time_point event_time;

    [[= Json() ]]
    SchemaJsonSettings settings;

    [[= Json() ]]
    std::string raw_json;

    [[= Lob() ]]
    std::string long_text;
};

std::chrono::system_clock::time_point utc_time(int year, int month, int day,
                                               int hour, int minute, int second) {
    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

TEST(SchemaGeneratorTest, ProducesDialectAwareDdl) {
    SqliteDialect sqlite;
    PostgresDialect postgres;

    const auto sqlite_sql = SchemaGenerator::create_table_sql<SchemaTestEntity>(sqlite);
    EXPECT_NE(sqlite_sql.find("id INTEGER PRIMARY KEY AUTOINCREMENT"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("email VARCHAR(120) NOT NULL UNIQUE"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("external_id TEXT"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("created_at TEXT"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("payload BLOB"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("status VARCHAR(255)"), std::string::npos);
    EXPECT_EQ(sqlite_sql.find("cache_only"), std::string::npos);

    const auto postgres_sql = SchemaGenerator::create_table_sql<SchemaTestEntity>(postgres);
    EXPECT_NE(postgres_sql.find("id BIGSERIAL PRIMARY KEY"), std::string::npos);
    EXPECT_NE(postgres_sql.find("external_id UUID"), std::string::npos);
    EXPECT_NE(postgres_sql.find("created_at TIMESTAMP"), std::string::npos);
    EXPECT_NE(postgres_sql.find("payload BYTEA"), std::string::npos);
}

TEST(SchemaGeneratorTest, MapsTemporalJsonAndLobDdl) {
    SqliteDialect sqlite;
    PostgresDialect postgres;

    const auto sqlite_sql = SchemaGenerator::create_table_sql<SchemaAnnotationEntity>(sqlite);
    EXPECT_NE(sqlite_sql.find("event_date TEXT"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("event_time TEXT"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("settings TEXT"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("raw_json TEXT"), std::string::npos);
    EXPECT_NE(sqlite_sql.find("long_text TEXT"), std::string::npos);

    const auto postgres_sql = SchemaGenerator::create_table_sql<SchemaAnnotationEntity>(postgres);
    EXPECT_NE(postgres_sql.find("event_date DATE"), std::string::npos);
    EXPECT_NE(postgres_sql.find("event_time TIME"), std::string::npos);
    EXPECT_NE(postgres_sql.find("settings JSONB"), std::string::npos);
    EXPECT_NE(postgres_sql.find("raw_json JSONB"), std::string::npos);
    EXPECT_NE(postgres_sql.find("long_text TEXT"), std::string::npos);
}

TEST(SchemaGeneratorTest, CreatesSqliteTable) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaTestEntity>(*datasource);

    auto conn = datasource->get_connection();
    auto result = conn->query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'schema_test_entities'");
    ASSERT_TRUE(result->next());
    EXPECT_EQ(result->get_string(0), "schema_test_entities");
}

TEST(SchemaGeneratorTest, PersistsTemporalJsonAndLobFields) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaAnnotationEntity>(*datasource);
    CrudRepository<SchemaAnnotationEntity, int> repository(datasource);

    SchemaAnnotationEntity entity;
    entity.event_date = utc_time(2026, 7, 17, 13, 14, 15);
    entity.event_time = utc_time(2026, 7, 17, 13, 14, 15);
    entity.settings = SchemaJsonSettings{.email = true, .level = 7};
    entity.raw_json = R"({"mode":"raw"})";
    entity.long_text = std::string(600, 'x');

    auto saved = repository.save(entity);

    auto conn = datasource->get_connection();
    auto raw = conn->query(
        "SELECT event_date, event_time, settings, raw_json, long_text "
        "FROM schema_annotation_entities WHERE id = ?", {Parameter(saved.id)});
    ASSERT_TRUE(raw->next());
    EXPECT_EQ(raw->get_string(0), "2026-07-17");
    EXPECT_EQ(raw->get_string(1), "13:14:15");
    EXPECT_NE(raw->get_string(2).find("\"email\":true"), std::string::npos);
    EXPECT_NE(raw->get_string(2).find("\"level\":7"), std::string::npos);
    EXPECT_EQ(raw->get_string(3), R"({"mode":"raw"})");
    EXPECT_EQ(raw->get_string(4).size(), 600);
    conn.reset();

    auto loaded = repository.find_by_id(saved.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->settings.email);
    EXPECT_EQ(loaded->settings.level, 7);
    EXPECT_EQ(loaded->raw_json, R"({"mode":"raw"})");
    EXPECT_EQ(loaded->long_text.size(), 600);
}

TEST(SchemaGeneratorTest, GeneratesManyToOneForeignKey) {
    SqliteDialect sqlite;
    PostgresDialect postgres;

    EXPECT_NE(SchemaGenerator::create_table_sql<SchemaArticle>(sqlite).find(
                  "author_id INTEGER REFERENCES schema_authors(id)"),
              std::string::npos);
    EXPECT_NE(SchemaGenerator::create_table_sql<SchemaArticle>(postgres).find(
                  "author_id INTEGER REFERENCES schema_authors(id)"),
              std::string::npos);
}

TEST(SchemaGeneratorTest, PersistsManyToOneForeignKey) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaAuthor>(*datasource);
    SchemaGenerator::create_table<SchemaArticle>(*datasource);

    CrudRepository<SchemaAuthor, int> authors(datasource);
    CrudRepository<SchemaArticle, int> articles(datasource);
    auto author = authors.save(SchemaAuthor{.name = "Ada"});
    auto article = articles.save(SchemaArticle{.author = author});

    auto loaded = articles.find_by_id(article.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->author.id, author.id);
    EXPECT_EQ(loaded->author.name, "Ada");
}

TEST(SchemaGeneratorTest, OneToManyIsNotStoredAsParentColumn) {
    SqliteDialect sqlite;
    const auto sql = SchemaGenerator::create_table_sql<SchemaBlog>(sqlite);
    EXPECT_NE(sql.find("id INTEGER PRIMARY KEY AUTOINCREMENT"), std::string::npos);
    EXPECT_NE(sql.find("title VARCHAR(255)"), std::string::npos);
    EXPECT_EQ(sql.find("posts"), std::string::npos);
}

TEST(SchemaGeneratorTest, EagerLoadsOneToManyThroughMappedByJoinColumn) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaBlog>(*datasource);
    SchemaGenerator::create_table<SchemaBlogPost>(*datasource);

    CrudRepository<SchemaBlog, int> blogs(datasource);
    CrudRepository<SchemaBlogPost, int> posts(datasource);

    SchemaBlog new_blog;
    new_blog.title = "Notes";
    auto blog = blogs.save(new_blog);
    posts.save(SchemaBlogPost{.title = "First", .blog = blog});
    posts.save(SchemaBlogPost{.title = "Second", .blog = blog});

    auto loaded = blogs.find_by_id(blog.id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->posts.size(), 2);
    EXPECT_EQ(loaded->posts[0].blog.id, blog.id);
    EXPECT_EQ(loaded->posts[0].blog.title, "");
    EXPECT_EQ(loaded->posts[0].title, "First");
    EXPECT_EQ(loaded->posts[1].title, "Second");
}

TEST(SchemaGeneratorTest, SavesOneToManyChildrenThroughMappedByOwner) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaBlog>(*datasource);
    SchemaGenerator::create_table<SchemaBlogPost>(*datasource);

    CrudRepository<SchemaBlog, int> blogs(datasource);

    SchemaBlog blog;
    blog.title = "Cascade";
    SchemaBlogPost first_child;
    first_child.title = "Child A";
    SchemaBlogPost second_child;
    second_child.title = "Child B";
    blog.posts.push_back(first_child);
    blog.posts.push_back(second_child);
    blog = blogs.save(blog);

    ASSERT_EQ(blog.posts.size(), 2);
    EXPECT_GT(blog.posts[0].id, 0);
    EXPECT_EQ(blog.posts[0].blog.id, blog.id);

    auto loaded = blogs.find_by_id(blog.id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->posts.size(), 2);
    EXPECT_EQ(loaded->posts[0].title, "Child A");
    EXPECT_EQ(loaded->posts[1].title, "Child B");
}

TEST(SchemaGeneratorTest, RemovesOneToManyOrphansWhenEnabled) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaBlog>(*datasource);
    SchemaGenerator::create_table<SchemaBlogPost>(*datasource);

    CrudRepository<SchemaBlog, int> blogs(datasource);
    CrudRepository<SchemaBlogPost, int> posts(datasource);

    SchemaBlog blog;
    blog.title = "Orphans";
    SchemaBlogPost first_child;
    first_child.title = "Keep";
    SchemaBlogPost second_child;
    second_child.title = "Remove";
    blog.posts.push_back(first_child);
    blog.posts.push_back(second_child);
    blog = blogs.save(blog);

    ASSERT_EQ(blog.posts.size(), 2);
    blog.posts.erase(blog.posts.begin() + 1);
    blog = blogs.save(blog);

    EXPECT_EQ(posts.count(), 1);
    auto loaded = blogs.find_by_id(blog.id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->posts.size(), 1);
    EXPECT_EQ(loaded->posts[0].title, "Keep");
}

TEST(SchemaGeneratorTest, DeletesOneToManyChildrenWhenRemoveCascades) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaBlog>(*datasource);
    SchemaGenerator::create_table<SchemaBlogPost>(*datasource);

    CrudRepository<SchemaBlog, int> blogs(datasource);
    CrudRepository<SchemaBlogPost, int> posts(datasource);

    SchemaBlog blog;
    blog.title = "Delete cascade";
    SchemaBlogPost child;
    child.title = "Child";
    blog.posts.push_back(child);
    blog = blogs.save(blog);

    ASSERT_EQ(posts.count(), 1);
    blogs.delete_by_id(blog.id);
    EXPECT_EQ(blogs.count(), 0);
    EXPECT_EQ(posts.count(), 0);
}

TEST(SchemaGeneratorTest, CreatesManyToManyJoinTableAndEagerLoadsRelatedRows) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaTag>(*datasource);
    SchemaGenerator::create_table<SchemaTaggedPost>(*datasource);

    auto conn = datasource->get_connection();
    auto join_table = conn->query(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'schema_post_tags'");
    ASSERT_TRUE(join_table->next());
    conn.reset();

    CrudRepository<SchemaTaggedPost, int> posts(datasource);

    SchemaTaggedPost post;
    post.title = "Associations";
    post.tags.push_back(SchemaTag{.label = "orm"});
    post.tags.push_back(SchemaTag{.label = "cpp"});
    post = posts.save(post);

    ASSERT_EQ(post.tags.size(), 2);
    EXPECT_GT(post.tags[0].id, 0);
    EXPECT_GT(post.tags[1].id, 0);

    auto loaded = posts.find_by_id(post.id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->tags.size(), 2);
    EXPECT_EQ(loaded->tags[0].label, "orm");
    EXPECT_EQ(loaded->tags[1].label, "cpp");
}

TEST(SchemaGeneratorTest, DeletesManyToManyJoinRowsButKeepsRelatedEntities) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    SchemaGenerator::create_table<SchemaTag>(*datasource);
    SchemaGenerator::create_table<SchemaTaggedPost>(*datasource);

    CrudRepository<SchemaTaggedPost, int> posts(datasource);
    CrudRepository<SchemaTag, int> tags(datasource);

    SchemaTaggedPost post;
    post.title = "Join cleanup";
    post.tags.push_back(SchemaTag{.label = "shared"});
    post = posts.save(post);

    auto conn = datasource->get_connection();
    auto rows_before = conn->query("SELECT COUNT(1) FROM schema_post_tags");
    ASSERT_TRUE(rows_before->next());
    ASSERT_EQ(rows_before->get_int(0), 1);
    conn.reset();

    posts.delete_by_id(post.id);

    conn = datasource->get_connection();
    auto rows_after = conn->query("SELECT COUNT(1) FROM schema_post_tags");
    ASSERT_TRUE(rows_after->next());
    EXPECT_EQ(rows_after->get_int(0), 0);
    conn.reset();
    EXPECT_EQ(tags.count(), 1);
}

TEST(SchemaGeneratorTest, RejectsExistingDifferentSchema) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = datasource->get_connection();
    conn->execute(R"(
        CREATE TABLE schema_test_entities (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            email TEXT NOT NULL,
            active BOOLEAN,
            balance REAL,
            external_id TEXT,
            created_at TEXT,
            payload BLOB,
            status VARCHAR(255)
        )
    )");
    conn.reset();

    EXPECT_THROW(SchemaGenerator::create_table<SchemaTestEntity>(*datasource),
                 SchemaMismatchException);
}

TEST(MigrationRunnerTest, AppliesEachMigrationOnce) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    int migration_runs = 0;
    const std::vector<Migration> migrations{
        Migration::sql(1, "create audit log", "CREATE TABLE audit_log (id INTEGER PRIMARY KEY, message TEXT)"),
        Migration{
            .version = 2,
            .description = "seed audit log",
            .apply = [&migration_runs](Connection& connection) {
                ++migration_runs;
                connection.execute("INSERT INTO audit_log (id, message) VALUES (1, 'created')");
            },
        },
    };

    MigrationRunner::run(*datasource, migrations);
    MigrationRunner::run(*datasource, migrations);
    EXPECT_EQ(migration_runs, 1);

    auto connection = datasource->get_connection();
    auto applied = connection->query("SELECT COUNT(1) FROM novaboot_schema_migrations");
    ASSERT_TRUE(applied->next());
    EXPECT_EQ(applied->get_int(0), 2);
}

TEST(MigrationRunnerTest, RejectsUndeclaredAppliedMigration) {
    auto datasource = std::make_shared<SqliteDataSource>(":memory:", 1);
    MigrationRunner::run(*datasource, {
        Migration::sql(1, "create audit log", "CREATE TABLE audit_log (id INTEGER PRIMARY KEY)"),
    });

    EXPECT_THROW(MigrationRunner::run(*datasource, {}), MigrationException);
}
