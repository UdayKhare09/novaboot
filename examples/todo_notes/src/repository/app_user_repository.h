#pragma once

#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "model/app_user.h"
#include <odb/query.hxx>
#include "app_user-odb.hxx"
#include <optional>

using namespace novaboot;
using namespace novaboot::data;
using todo_notes::model::AppUser;

struct AppUserRepository : public PgsqlRepositoryBase<AppUser, int> {
public:
    explicit AppUserRepository(PgsqlDataSource& ds)
        : PgsqlRepositoryBase<AppUser, int>(ds) {}

    std::optional<AppUser> find_by_username(const std::string& username) {
        return ds_.transact([&](auto& db) -> std::optional<AppUser> {
            typedef odb::query<AppUser> query;
            auto result = db.template query<AppUser>(query::username == username);
            auto it = result.begin();
            if (it != result.end()) {
                return *it;
            }
            return std::nullopt;
        });
    }

    std::optional<AppUser> find_by_email(const std::string& email) {
        return ds_.transact([&](auto& db) -> std::optional<AppUser> {
            typedef odb::query<AppUser> query;
            auto result = db.template query<AppUser>(query::email == email);
            auto it = result.begin();
            if (it != result.end()) {
                return *it;
            }
            return std::nullopt;
        });
    }
};
