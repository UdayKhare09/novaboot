#pragma once

#include "model/app_user.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/db_client.h"
#include <vector>
#include <optional>
#include <string>
#include <memory>

using namespace novaboot::annotations;
using todo_notes::model::AppUser;

struct [[= Repository() ]] AppUserRepository : public novaboot::db::CrudRepository<AppUser, std::string> {
public:
    explicit AppUserRepository(std::shared_ptr<novaboot::db::DataSource> ds)
        : novaboot::db::CrudRepository<AppUser, std::string>(ds) {}

    std::optional<AppUser> find_by_username(const std::string& username) {
        return query()
            .where<&AppUser::username>(novaboot::db::Op::Equal, username)
            .single();
    }

    std::optional<AppUser> find_by_email(const std::string& email) {
        return query()
            .where<&AppUser::email>(novaboot::db::Op::Equal, email)
            .single();
    }
};
