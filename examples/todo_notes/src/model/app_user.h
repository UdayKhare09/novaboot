#pragma once
#include <string>

#ifndef ODB_COMPILER
#include "novaboot/validation/validation.h"
#else
#include <odb/core.hxx>
#endif

namespace todo_notes::model {

#ifndef ODB_COMPILER
using novaboot::validation::Schema;
#endif

#pragma db object table("app_users")
struct AppUser {
#pragma db id
    std::string id;

    std::string username;
    std::string password_hash;
    std::string email;

#ifndef ODB_COMPILER
    inline static const Schema<AppUser> validator =
        Schema<AppUser>()
            .field<&AppUser::username>("username").not_empty().size(3, 30)
            .field<&AppUser::email>("email").email();
#endif
};

} // namespace todo_notes::model
