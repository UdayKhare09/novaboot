#pragma once
#include <string>

#ifndef ODB_COMPILER
#include "novaboot/validation/validation.h"
#else
#include <odb/core.hxx>
#endif

namespace examples::model {

#ifndef ODB_COMPILER
using novaboot::validation::Schema;

struct is_valid_role {
    char prefix[32] = {};

    consteval is_valid_role() {
        const char* p = "ROLE_";
        int i = 0;
        while (p[i] && i < 31) {
            prefix[i] = p[i];
            i++;
        }
        prefix[i] = '\0';
    }

    consteval is_valid_role(const char* p) {
        int i = 0;
        while (p[i] && i < 31) {
            prefix[i] = p[i];
            i++;
        }
        prefix[i] = '\0';
    }

    bool validate(const std::string& val, std::string& err) const {
        std::string pref(prefix);
        if (val.find(pref) != 0) {
            err = "must start with " + pref;
            return false;
        }
        return true;
    }
};
#endif

#pragma db object table("users")
struct User {
#pragma db id
    int id;

    std::string name;

    std::string email;

    std::string role;

#ifndef ODB_COMPILER
    inline static const Schema<User> validator =
        Schema<User>()
            .field<&User::id>("id").min(0)
            .field<&User::name>("name").not_empty().size(2, 20)
            .field<&User::email>("email").email()
            .field<&User::role>("role").custom(is_valid_role());
#endif
};

} // namespace examples::model
