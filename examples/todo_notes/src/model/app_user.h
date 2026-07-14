#pragma once
#include <string>
#include "novaboot/validation/validation.h"

namespace todo_notes::model {

using novaboot::validation::Schema;

struct AppUser {
    std::string id;
    std::string username;
    std::string password_hash;
    std::string email;

    inline static const Schema<AppUser> validator =
        Schema<AppUser>()
            .field<&AppUser::username>("username").not_empty().size(3, 30)
            .field<&AppUser::email>("email").email();
};

} // namespace todo_notes::model
