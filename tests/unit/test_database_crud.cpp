#include <gtest/gtest.h>
#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/transaction.h"
#include <vector>
#include <string>
#include <algorithm>

using namespace novaboot;
using namespace novaboot::db;
using namespace novaboot::db::sqlite;
using namespace novaboot::annotations;

struct [[= Entity("test_entities") ]] DBTestEntity {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;
    
    std::string name;
    int score = 0;
};

struct TestEntityRepository : public CrudRepository<DBTestEntity, int> {
    explicit TestEntityRepository(std::shared_ptr<DataSource> ds) 
        : CrudRepository<DBTestEntity, int>(ds) {}
        
    std::vector<DBTestEntity> find_by_score_above(int score) {
        return query()
            .where<&DBTestEntity::score>(Op::GreaterThan, score)
            .order_by<&DBTestEntity::score>(false /* desc */)
            .list();
    }
};

struct [[= Entity("query_predicate_entities") ]] QueryPredicateEntity {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;

    std::string name;
    int score = 0;
    std::string optional_label;
};

struct QueryPredicateRepository : public CrudRepository<QueryPredicateEntity, int> {
    explicit QueryPredicateRepository(std::shared_ptr<DataSource> ds)
        : CrudRepository<QueryPredicateEntity, int>(ds) {}
};

struct QueryNameScoreProjection {
    std::string name;
    int score = 0;
};

struct [[= Entity("query_authors") ]] QueryAuthor {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;

    std::string name;
};

struct [[= Entity("query_articles") ]] QueryArticle {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;

    std::string title;

    [[= ManyToOne() ]]
    [[= JoinColumn("author_id") ]]
    QueryAuthor author;
};

TEST(DatabaseCrudTest, LifecycleAndQuery) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);

    // Create table
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE test_entities (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            score INTEGER NOT NULL
        );
    )");
    conn.reset();

    TestEntityRepository repo(ds);

    // 1. Save new entities
    DBTestEntity e1{0, "Bob", 85};
    DBTestEntity e2{0, "Alice", 95};

    auto s1 = repo.save(e1);
    auto s2 = repo.save(e2);

    // Verify auto-increment IDs populated
    EXPECT_EQ(s1.id, 1);
    EXPECT_EQ(s2.id, 2);

    // 2. Find by id
    auto f1 = repo.find_by_id(1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->name, "Bob");
    EXPECT_EQ(f1->score, 85);

    // 3. Exists by id
    EXPECT_TRUE(repo.exists_by_id(2));
    EXPECT_FALSE(repo.exists_by_id(3));

    // 4. Update
    s1.score = 90;
    repo.save(s1);
    auto f1_updated = repo.find_by_id(1);
    ASSERT_TRUE(f1_updated.has_value());
    EXPECT_EQ(f1_updated->score, 90);

    // 5. Custom Query / Sort / Limit
    auto matches = repo.find_by_score_above(88);
    // Alice (95) and Bob (90) should both match, sorted by score desc (Alice then Bob)
    ASSERT_EQ(matches.size(), 2);
    EXPECT_EQ(matches[0].name, "Alice");
    EXPECT_EQ(matches[1].name, "Bob");

    // 6. Delete
    repo.delete_by_id(1);
    EXPECT_FALSE(repo.exists_by_id(1));
    EXPECT_TRUE(repo.exists_by_id(2));
}

TEST(DatabaseCrudTest, RepositoryCollectionPrimitives) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE test_entities (
            score INTEGER NOT NULL,
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL
        );
    )");
    conn.reset();

    TestEntityRepository repo(ds);
    auto saved = repo.save_all({
        DBTestEntity{0, "Alice", 95},
        DBTestEntity{0, "Bob", 85},
    });

    ASSERT_EQ(saved.size(), 2);
    EXPECT_EQ(repo.count(), 2);

    auto selected = repo.find_all_by_id({saved[1].id, saved[0].id, 999});
    ASSERT_EQ(selected.size(), 2);
    auto alice = std::find_if(selected.begin(), selected.end(), [](const auto& entity) {
        return entity.name == "Alice";
    });
    auto bob = std::find_if(selected.begin(), selected.end(), [](const auto& entity) {
        return entity.name == "Bob";
    });
    ASSERT_NE(alice, selected.end());
    ASSERT_NE(bob, selected.end());
    EXPECT_EQ(alice->score, 95);
    EXPECT_EQ(bob->score, 85);

    repo.delete_entity(saved[0]);
    EXPECT_EQ(repo.count(), 1);
    EXPECT_FALSE(repo.exists_by_id(saved[0].id));
}

TEST(DatabaseCrudTest, QueryPredicatesAndGroups) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE query_predicate_entities (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL,
            score INTEGER NOT NULL,
            optional_label TEXT
        );
    )");
    conn->execute("INSERT INTO query_predicate_entities VALUES (1, 'Alice', 95, 'team-a')");
    conn->execute("INSERT INTO query_predicate_entities VALUES (2, 'Bob', 85, NULL)");
    conn->execute("INSERT INTO query_predicate_entities VALUES (3, 'Carol', 70, 'team-c')");
    conn.reset();

    QueryPredicateRepository repo(ds);

    auto ranged = repo.query()
        .where_between<&QueryPredicateEntity::score>(80, 90)
        .and_in<&QueryPredicateEntity::name>(std::vector<std::string>{"Alice", "Bob"})
        .list();
    ASSERT_EQ(ranged.size(), 1);
    EXPECT_EQ(ranged.front().name, "Bob");

    auto null_values = repo.query()
        .where_is_null<&QueryPredicateEntity::optional_label>()
        .list();
    ASSERT_EQ(null_values.size(), 1);
    EXPECT_EQ(null_values.front().name, "Bob");

    auto non_null_values = repo.query()
        .where_is_not_null<&QueryPredicateEntity::optional_label>()
        .list();
    EXPECT_EQ(non_null_values.size(), 2);

    auto grouped = repo.query()
        .where_group([](QueryBuilder<QueryPredicateEntity>& group) {
            group.where<&QueryPredicateEntity::score>(Op::GreaterThan, 80)
                 .or_<&QueryPredicateEntity::score>(Op::Equal, 70);
        })
        .and_<&QueryPredicateEntity::name>(Op::Like, std::string("A%"))
        .single();
    ASSERT_TRUE(grouped.has_value());
    EXPECT_EQ(grouped->name, "Alice");

    auto empty_membership = repo.query()
        .where_in<&QueryPredicateEntity::id>(std::vector<int>{})
        .list();
    EXPECT_TRUE(empty_membership.empty());

    auto reset_where = repo.query();
    auto reset_result = reset_where
        .where<&QueryPredicateEntity::score>(Op::GreaterThan, 0)
        .where<&QueryPredicateEntity::name>(Op::Equal, std::string("Alice"))
        .single();
    ASSERT_TRUE(reset_result.has_value());
    EXPECT_EQ(reset_result->name, "Alice");

    auto page = repo.query().page(Pageable{
        .page = 1,
        .size = 1,
        .sort = {{.column = "name", .ascending = true}},
    });
    ASSERT_EQ(page.content.size(), 1);
    EXPECT_EQ(page.content.front().name, "Bob");
    EXPECT_EQ(page.total_elements, 3);
    EXPECT_EQ(page.total_pages(), 3);
    EXPECT_TRUE(page.has_next());

    EXPECT_THROW(repo.query().page(Pageable{.page = 0, .size = 1,
                                             .sort = {{.column = "invalid", .ascending = true}}}),
                 std::invalid_argument);
}

TEST(DatabaseCrudTest, QueryProjectionsCountAndExists) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE query_predicate_entities (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL,
            score INTEGER NOT NULL,
            optional_label TEXT
        );
    )");
    conn->execute("INSERT INTO query_predicate_entities VALUES (1, 'Alice', 95, 'team-a')");
    conn->execute("INSERT INTO query_predicate_entities VALUES (2, 'Bob', 85, NULL)");
    conn->execute("INSERT INTO query_predicate_entities VALUES (3, 'Carol', 70, 'team-c')");
    conn.reset();

    QueryPredicateRepository repo(ds);
    auto query = repo.query()
        .where<&QueryPredicateEntity::score>(Op::GreaterThan, 80)
        .order_by<&QueryPredicateEntity::name>();

    EXPECT_EQ(query.count(), 2);
    EXPECT_TRUE(query.exists());

    auto projections = query.project<QueryNameScoreProjection>();
    ASSERT_EQ(projections.size(), 2);
    EXPECT_EQ(projections[0].name, "Alice");
    EXPECT_EQ(projections[0].score, 95);
    EXPECT_EQ(projections[1].name, "Bob");

    EXPECT_FALSE(repo.query()
        .where<&QueryPredicateEntity::name>(Op::Equal, std::string("Nobody"))
        .exists());
}

TEST(DatabaseCrudTest, QueryManyToOnePredicateBindsRelatedIdentifier) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE query_authors (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL
        );
    )");
    conn->execute(R"(
        CREATE TABLE query_articles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            author_id INTEGER NOT NULL
        );
    )");
    conn.reset();

    CrudRepository<QueryAuthor, int> authors(ds);
    CrudRepository<QueryArticle, int> articles(ds);

    auto ada = authors.save(QueryAuthor{.name = "Ada"});
    auto grace = authors.save(QueryAuthor{.name = "Grace"});
    articles.save(QueryArticle{.title = "A", .author = ada});
    articles.save(QueryArticle{.title = "B", .author = grace});
    articles.save(QueryArticle{.title = "C", .author = ada});

    auto ada_articles = articles.query()
        .where<&QueryArticle::author>(Op::Equal, ada)
        .order_by<&QueryArticle::title>()
        .list();

    ASSERT_EQ(ada_articles.size(), 2);
    EXPECT_EQ(ada_articles[0].title, "A");
    EXPECT_EQ(ada_articles[1].title, "C");
    EXPECT_EQ(articles.query().where<&QueryArticle::author>(Op::Equal, grace).count(), 1);
}

TEST(DatabaseCrudTest, TransactionCommitAndRollback) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE test_entities (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            score INTEGER NOT NULL
        );
    )");
    conn.reset();

    TestEntityRepository outside_transaction(ds);
    {
        Transaction transaction(ds);
        CrudRepository<DBTestEntity, int> repository(ds, transaction.connection());
        repository.save(DBTestEntity{0, "Committed", 10});
        EXPECT_EQ(repository.count(), 1);
        transaction.commit();
    }
    EXPECT_EQ(outside_transaction.count(), 1);

    {
        Transaction transaction(ds);
        CrudRepository<DBTestEntity, int> repository(ds, transaction.connection());
        repository.save(DBTestEntity{0, "Rolled back", 20});
        EXPECT_EQ(repository.count(), 2);
        // Destructor performs rollback when commit() is not called.
    }
    EXPECT_EQ(outside_transaction.count(), 1);
}
