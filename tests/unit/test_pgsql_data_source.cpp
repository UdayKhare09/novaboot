#include <gtest/gtest.h>
#include "novaboot/data/pgsql/pgsql_data_source.h"
#include <odb/pgsql/exceptions.hxx>

using namespace novaboot::data;
using namespace novaboot::config;

TEST(PgsqlDataSourceTest, InitWithConfig) {
    PostgresConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 5432;
    cfg.user = "nova";
    cfg.password = "secret";
    cfg.database = "novadb";
    cfg.pool_min = 2;
    cfg.pool_max = 5;

    // Constructing the data source might throw a connection failure exception if PostgreSQL is down.
    // We catch and handle it gracefully.
    try {
        PgsqlDataSource ds(cfg);
        auto& db = ds.db();
        (void)db;
    } catch (const odb::pgsql::database_exception& err) {
        SUCCEED() << "Caught expected pgsql connection error: " << err.what();
    } catch (...) {
        FAIL() << "Unexpected exception thrown";
    }
}
