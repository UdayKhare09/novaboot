#pragma once
#include <string>
#include "novaboot/validation/validation.h"
#include "novaboot/annotations/stereotypes.h"

namespace todo_notes::model {

using novaboot::validation::Schema;
using namespace novaboot::annotations;

struct [[= Entity("users") ]] AppUser {
    [[= Id() ]]
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
