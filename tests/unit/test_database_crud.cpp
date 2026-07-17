#include <gtest/gtest.h>
#include "novaboot/db/drivers/sqlite/sqlite_driver.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/transaction.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <thread>

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

enum class QueryTicketStatus { Open, Closed, Archived };

struct [[= Entity("query_tickets") ]] QueryTicket {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;

    std::string title;

    [[= Enumerated(EnumType::String) ]]
    QueryTicketStatus status = QueryTicketStatus::Open;
};

struct QueryTicketRepository : public CrudRepository<QueryTicket, int> {
    explicit QueryTicketRepository(std::shared_ptr<DataSource> ds)
        : CrudRepository<QueryTicket, int>(ds) {}
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

struct [[= Entity("lazy_authors") ]] LazyAuthor {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;

    std::string name;
};

struct [[= Entity("lazy_articles") ]] LazyArticle {
    [[= Id() ]]
    [[= GeneratedValue() ]]
    int id = 0;

    std::string title;

    [[= ManyToOne(FetchType::Lazy) ]]
    [[= JoinColumn("author_id") ]]
    Lazy<LazyAuthor> author;
};

struct TransactionalCrudService {
    TransactionManager& transactions;
    std::shared_ptr<DataSource> datasource;

    TransactionalCrudService(TransactionManager& tx, std::shared_ptr<DataSource> ds)
        : transactions(tx), datasource(std::move(ds)) {}

    [[= Transactional() ]]
    DBTestEntity create_committed(std::string name, int score) {
        return transactions.execute([&](std::shared_ptr<Connection> connection) {
            CrudRepository<DBTestEntity, int> repo(datasource, connection);
            return repo.save(DBTestEntity{.name = std::move(name), .score = score});
        });
    }

    [[= Transactional() ]]
    void create_then_fail() {
        transactions.execute([&](std::shared_ptr<Connection> connection) {
            CrudRepository<DBTestEntity, int> repo(datasource, connection);
            repo.save(DBTestEntity{.name = "rolled back", .score = 1});
            throw std::runtime_error("boom");
        });
    }
};

struct TransactionalInvokeService {
    CrudRepository<DBTestEntity, int>& repo;

    explicit TransactionalInvokeService(CrudRepository<DBTestEntity, int>& repository)
        : repo(repository) {}

    [[= Transactional() ]]
    DBTestEntity create(std::string name, int score) {
        return repo.save(DBTestEntity{.name = std::move(name), .score = score});
    }

    [[= Transactional() ]]
    void create_then_fail() {
        repo.save(DBTestEntity{.name = "invoke rollback", .score = 1});
        throw std::runtime_error("rollback");
    }

    DBTestEntity create_without_transaction(std::string name, int score) {
        return repo.save(DBTestEntity{.name = std::move(name), .score = score});
    }
};

static_assert(novaboot::di::detail::has_annotation<Transactional>(
                  ^^TransactionalCrudService::create_committed),
              "Transactional annotation should be reflected on service methods");

struct TransactionMetadataProbe {
    [[= Transactional(TransactionPropagation::RequiresNew,
                      TransactionIsolation::Serializable,
                      true,
                      7) ]]
    void run() {}
};

static_assert(novaboot::di::detail::get_annotation<Transactional>(
                  ^^TransactionMetadataProbe::run).propagation ==
                  TransactionPropagation::RequiresNew);
static_assert(novaboot::di::detail::get_annotation<Transactional>(
                  ^^TransactionMetadataProbe::run).isolation ==
                  TransactionIsolation::Serializable);
static_assert(novaboot::di::detail::get_annotation<Transactional>(
                  ^^TransactionMetadataProbe::run).read_only);
static_assert(novaboot::di::detail::get_annotation<Transactional>(
                  ^^TransactionMetadataProbe::run).timeout_seconds == 7);

struct CommitAnywayError : std::runtime_error {
    CommitAnywayError() : std::runtime_error("commit anyway") {}
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

TEST(DatabaseCrudTest, TypedRepositoryFieldShortcuts) {
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

    CrudRepository<DBTestEntity, int> repo(ds);
    repo.save(DBTestEntity{.name = "Ada", .score = 10});
    repo.save(DBTestEntity{.name = "Ada", .score = 20});
    repo.save(DBTestEntity{.name = "Grace", .score = 30});

    auto one = repo.find_one_by<&DBTestEntity::name>("Ada");
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->name, "Ada");

    auto all_ada = repo.find_all_by<&DBTestEntity::name>("Ada");
    ASSERT_EQ(all_ada.size(), 2);
    EXPECT_TRUE(repo.exists_by<&DBTestEntity::score>(30));
    EXPECT_FALSE(repo.exists_by<&DBTestEntity::score>(404));
    EXPECT_EQ(repo.count_by<&DBTestEntity::name>("Ada"), 2);

    EXPECT_EQ(repo.delete_by<&DBTestEntity::name>("Ada"), 2);
    EXPECT_EQ(repo.count(), 1);
    auto grace = repo.find_one_by<&DBTestEntity::name>("Grace");
    ASSERT_TRUE(grace.has_value());
    EXPECT_EQ(grace->score, 30);
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

    auto grace_articles = articles.query()
        .where_related<&QueryArticle::author, &QueryAuthor::name>(Op::Equal, "Grace")
        .list();
    ASSERT_EQ(grace_articles.size(), 1);
    EXPECT_EQ(grace_articles[0].title, "B");

    const auto author_name_matches = articles.query()
        .where_related<&QueryArticle::author, &QueryAuthor::name>(Op::Like, "A%")
        .count();
    EXPECT_EQ(author_name_matches, 2);

    auto sorted_by_author = articles.query()
        .order_by_related<&QueryArticle::author, &QueryAuthor::name>()
        .order_by<&QueryArticle::title>()
        .list();
    ASSERT_EQ(sorted_by_author.size(), 3);
    EXPECT_EQ(sorted_by_author[0].title, "A");
    EXPECT_EQ(sorted_by_author[1].title, "C");
    EXPECT_EQ(sorted_by_author[2].title, "B");
}

TEST(DatabaseCrudTest, QueryEnumPredicateBindsStringBackedEnumName) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE query_tickets (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            status TEXT NOT NULL
        );
    )");
    conn.reset();

    QueryTicketRepository repo(ds);
    repo.save(QueryTicket{.title = "one", .status = QueryTicketStatus::Open});
    repo.save(QueryTicket{.title = "two", .status = QueryTicketStatus::Closed});
    repo.save(QueryTicket{.title = "three", .status = QueryTicketStatus::Closed});

    auto closed = repo.query()
        .where<&QueryTicket::status>(Op::Equal, QueryTicketStatus::Closed)
        .order_by<&QueryTicket::title>()
        .list();

    ASSERT_EQ(closed.size(), 2);
    EXPECT_EQ(closed[0].title, "three");
    EXPECT_EQ(closed[1].title, "two");
    EXPECT_EQ(repo.query().where<&QueryTicket::status>(Op::Equal, QueryTicketStatus::Archived).count(), 0);

    auto open_or_closed = repo.query()
        .where_in<&QueryTicket::status>(std::vector<QueryTicketStatus>{
            QueryTicketStatus::Open,
            QueryTicketStatus::Closed,
        })
        .count();
    EXPECT_EQ(open_or_closed, 3);
}

TEST(DatabaseCrudTest, LazyManyToOneLoadsOnFirstAccess) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE lazy_authors (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL
        );
    )");
    conn->execute(R"(
        CREATE TABLE lazy_articles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            author_id INTEGER NOT NULL
        );
    )");
    conn.reset();

    CrudRepository<LazyAuthor, int> authors(ds);
    CrudRepository<LazyArticle, int> articles(ds);

    auto ada = authors.save(LazyAuthor{.name = "Ada"});
    LazyArticle draft;
    draft.title = "Deferred relations";
    draft.author = Lazy<LazyAuthor>::loaded(ada, Parameter(static_cast<std::int64_t>(ada.id)));
    auto saved = articles.save(draft);

    auto reloaded = articles.find_by_id(saved.id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_FALSE(reloaded->author.loaded());
    EXPECT_TRUE(reloaded->author.has_identity());

    const auto& author = reloaded->author.get();
    EXPECT_TRUE(reloaded->author.loaded());
    EXPECT_EQ(author.id, ada.id);
    EXPECT_EQ(author.name, "Ada");

    auto fetched = articles.query()
        .fetch<&LazyArticle::author>()
        .where<&LazyArticle::id>(Op::Equal, saved.id)
        .single();
    ASSERT_TRUE(fetched.has_value());
    EXPECT_TRUE(fetched->author.loaded());
    EXPECT_EQ(fetched->author->id, ada.id);
    EXPECT_EQ(fetched->author->name, "Ada");
}

TEST(DatabaseCrudTest, TransactionBoundLazyManyToOneUsesPinnedConnection) {
    auto ds = std::make_shared<SqliteDataSource>(":memory:", 1);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE lazy_authors (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL
        );
    )");
    conn->execute(R"(
        CREATE TABLE lazy_articles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            author_id INTEGER NOT NULL
        );
    )");
    conn.reset();

    Transaction transaction(ds);
    CrudRepository<LazyAuthor, int> authors(ds, transaction.connection());
    CrudRepository<LazyArticle, int> articles(ds, transaction.connection());

    auto ada = authors.save(LazyAuthor{.name = "Ada in tx"});
    LazyArticle draft;
    draft.title = "Same connection";
    draft.author = Lazy<LazyAuthor>::loaded(ada, Parameter(static_cast<std::int64_t>(ada.id)));
    auto saved = articles.save(draft);

    auto reloaded = articles.find_by_id(saved.id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_FALSE(reloaded->author.loaded());

    const auto& author = reloaded->author.get();
    EXPECT_TRUE(reloaded->author.loaded());
    EXPECT_EQ(author.id, ada.id);
    EXPECT_EQ(author.name, "Ada in tx");

    transaction.rollback();
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

TEST(DatabaseCrudTest, TransactionManagerCommitsAndRollsBackCallbacks) {
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

    TransactionManager transactions(ds);
    TransactionalCrudService service(transactions, ds);
    CrudRepository<DBTestEntity, int> outside_transaction(ds);

    auto saved = service.create_committed("committed", 42);
    EXPECT_GT(saved.id, 0);
    EXPECT_EQ(outside_transaction.count(), 1);

    EXPECT_THROW(service.create_then_fail(), std::runtime_error);
    EXPECT_EQ(outside_transaction.count(), 1);
}

TEST(DatabaseCrudTest, TransactionManagerInvokeWrapsAnnotatedServiceMethods) {
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

    TransactionManager transactions(ds);
    CrudRepository<DBTestEntity, int> repo(ds);
    TransactionalInvokeService service(repo);

    auto saved = transactions.invoke<&TransactionalInvokeService::create>(
        service, std::string("invoke committed"), 42);
    EXPECT_GT(saved.id, 0);
    EXPECT_EQ(repo.count(), 1);

    EXPECT_THROW(transactions.invoke<&TransactionalInvokeService::create_then_fail>(service),
                 std::runtime_error);
    EXPECT_EQ(repo.count(), 1);

    EXPECT_THROW(service.create_then_fail(), std::runtime_error);
    EXPECT_EQ(repo.count(), 2) << "Direct C++ service calls are not interceptable; use TransactionManager::invoke or generated dispatch";
}

TEST(DatabaseCrudTest, TransactionManagerInvokeLeavesUnannotatedMethodsPlain) {
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

    TransactionManager transactions(ds);
    CrudRepository<DBTestEntity, int> repo(ds);
    TransactionalInvokeService service(repo);

    auto saved = transactions.invoke<&TransactionalInvokeService::create_without_transaction>(
        service, std::string("plain"), 9);
    EXPECT_GT(saved.id, 0);
    EXPECT_EQ(repo.count(), 1);
}

TEST(DatabaseCrudTest, TransactionManagerRequiredReusesActiveConnection) {
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

    TransactionManager transactions(ds);
    transactions.execute([&](std::shared_ptr<Connection> outer_connection) {
        CrudRepository<DBTestEntity, int> outer_repo(ds, outer_connection);
        outer_repo.save(DBTestEntity{.name = "outer", .score = 1});

        transactions.execute([&](std::shared_ptr<Connection> inner_connection) {
            EXPECT_EQ(inner_connection.get(), outer_connection.get());
            CrudRepository<DBTestEntity, int> inner_repo(ds, inner_connection);
            inner_repo.save(DBTestEntity{.name = "inner", .score = 2});
        });
    });

    CrudRepository<DBTestEntity, int> repo(ds);
    EXPECT_EQ(repo.count(), 2);
}

TEST(DatabaseCrudTest, RepositoriesJoinAmbientTransaction) {
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

    TransactionManager transactions(ds);
    CrudRepository<DBTestEntity, int> repo(ds);

    EXPECT_THROW(transactions.execute([&](std::shared_ptr<Connection>) {
        repo.save(DBTestEntity{.name = "ambient rollback", .score = 1});
        throw std::runtime_error("rollback");
    }), std::runtime_error);

    EXPECT_EQ(repo.count(), 0);
}

TEST(DatabaseCrudTest, TransactionManagerNoRollbackForCommitsThenRethrows) {
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

    TransactionManager transactions(ds);
    TransactionOptions options;
    options.no_rollback_for<CommitAnywayError>();

    EXPECT_THROW(transactions.execute(options, [&](std::shared_ptr<Connection> connection) {
        CrudRepository<DBTestEntity, int> repo(ds, connection);
        repo.save(DBTestEntity{.name = "committed despite exception", .score = 99});
        throw CommitAnywayError();
    }), CommitAnywayError);

    CrudRepository<DBTestEntity, int> repo(ds);
    EXPECT_EQ(repo.count(), 1);
}

TEST(DatabaseCrudTest, TransactionManagerRollbackForRestrictsRollbackTypes) {
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

    TransactionManager transactions(ds);
    TransactionOptions options;
    options.rollback_for<CommitAnywayError>();

    EXPECT_THROW(transactions.execute(options, [&](std::shared_ptr<Connection> connection) {
        CrudRepository<DBTestEntity, int> repo(ds, connection);
        repo.save(DBTestEntity{.name = "committed runtime_error", .score = 11});
        throw std::runtime_error("not listed");
    }), std::runtime_error);

    CrudRepository<DBTestEntity, int> repo(ds);
    EXPECT_EQ(repo.count(), 1);

    EXPECT_THROW(transactions.execute(options, [&](std::shared_ptr<Connection> connection) {
        CrudRepository<DBTestEntity, int> scoped_repo(ds, connection);
        scoped_repo.save(DBTestEntity{.name = "rolled back listed", .score = 12});
        throw CommitAnywayError();
    }), CommitAnywayError);

    EXPECT_EQ(repo.count(), 1);
}

TEST(DatabaseCrudTest, TransactionManagerTimeoutRollsBack) {
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

    TransactionManager transactions(ds);
    TransactionOptions options;
    options.timeout_seconds = 1;

    EXPECT_THROW(transactions.execute(options, [&](std::shared_ptr<Connection> connection) {
        CrudRepository<DBTestEntity, int> repo(ds, connection);
        repo.save(DBTestEntity{.name = "too slow", .score = 1});
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }), TransactionTimeoutException);

    CrudRepository<DBTestEntity, int> repo(ds);
    EXPECT_EQ(repo.count(), 0);
}

TEST(DatabaseCrudTest, TransactionManagerRequiresNewCommitsInnerWhenOuterRollsBack) {
    const char* path = "/tmp/novaboot_tx_requires_new.sqlite";
    std::remove(path);

    auto ds = std::make_shared<SqliteDataSource>(path, 2);
    auto conn = ds->get_connection();
    conn->execute(R"(
        CREATE TABLE test_entities (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            score INTEGER NOT NULL
        );
    )");
    conn.reset();

    TransactionManager transactions(ds);
    TransactionOptions requires_new;
    requires_new.propagation = TransactionPropagation::RequiresNew;

    EXPECT_THROW(transactions.execute([&](std::shared_ptr<Connection>) {
        transactions.execute(requires_new, [&](std::shared_ptr<Connection> inner_connection) {
            CrudRepository<DBTestEntity, int> inner_repo(ds, inner_connection);
            inner_repo.save(DBTestEntity{.name = "inner committed", .score = 7});
        });
        throw std::runtime_error("outer rollback");
    }), std::runtime_error);

    CrudRepository<DBTestEntity, int> repo(ds);
    EXPECT_EQ(repo.count(), 1);
    auto saved = repo.find_one_by<&DBTestEntity::name>("inner committed");
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->score, 7);

    ds->close();
    std::remove(path);
}
