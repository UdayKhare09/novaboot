#pragma once

#include "novaboot/config/app_config.h"
#include <odb/pgsql/database.hxx>
#include <odb/pgsql/connection-factory.hxx>
#include <odb/transaction.hxx>
#include <memory>

namespace novaboot::data {

class PgsqlDataSource {
public:
    explicit PgsqlDataSource(const config::PostgresConfig& cfg);
    ~PgsqlDataSource() = default;

    // Get or lease a pooled ODB connection
    odb::pgsql::database& db() { return *db_; }

    // Execute in a transaction (RAII)
    template<typename F>
    auto transact(F&& fn) -> decltype(fn(db())) {
        odb::transaction t(db_->begin());
        if constexpr (std::is_void_v<decltype(fn(db()))>) {
            fn(db());
            t.commit();
        } else {
            auto res = fn(db());
            t.commit();
            return res;
        }
    }

private:
    std::unique_ptr<odb::pgsql::database> db_;
};

} // namespace novaboot::data
