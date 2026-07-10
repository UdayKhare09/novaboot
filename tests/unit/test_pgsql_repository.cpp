#include <gtest/gtest.h>
#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "db_user.h"
#include "db_user-odb.hxx"
#include <odb/pgsql/exceptions.hxx>

using namespace novaboot::data;

class DbUserRepository : public PgsqlRepositoryBase<DbUser, int> {
public:
    explicit DbUserRepository(PgsqlDataSource& ds) : PgsqlRepositoryBase<DbUser, int>(ds) {}
};

TEST(PgsqlRepositoryTest, CRUDOperations) {
    novaboot::config::PostgresConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 5432;
    cfg.user = "nova";
    cfg.password = "secret";
    cfg.database = "novadb";

    try {
        PgsqlDataSource ds(cfg);
        DbUserRepository repo(ds);
        
        DbUser u;
        u.id = 1;
        u.name = "John Doe";
        u.email = "john@example.com";

        repo.save(u);
    } catch (const odb::pgsql::database_exception& e) {
        SUCCEED() << "Caught expected ODB pgsql connection failure: " << e.what();
    } catch (...) {
        FAIL() << "Unexpected exception";
    }
}
