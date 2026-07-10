#include "novaboot/data/pgsql/pgsql_data_source.h"

namespace novaboot::data {

PgsqlDataSource::PgsqlDataSource(const config::PostgresConfig& cfg) {
    std::unique_ptr<odb::pgsql::connection_factory> factory = 
        std::make_unique<odb::pgsql::connection_pool_factory>(
            cfg.pool_max,
            cfg.pool_min
        );

    db_ = std::make_unique<odb::pgsql::database>(
        cfg.user,
        cfg.password,
        cfg.database,
        cfg.host,
        cfg.port,
        "",  // extra_conninfo
        std::move(factory)
    );
}

} // namespace novaboot::data
