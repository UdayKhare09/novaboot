#pragma once
#include <string>

#include "novaboot/validation/validation.h"

namespace examples::model {

#ifndef ODB_COMPILER
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

struct User {
    int id;

    std::string name;

    std::string email;

    std::string role;

    inline static const novaboot::validation::Schema<User> validator =
        novaboot::validation::Schema<User>()
            .field<&User::id>("id").min(0)
            .field<&User::name>("name").not_empty().size(2, 20)
            .field<&User::email>("email").email()
            .field<&User::role>("role").custom(is_valid_role());

};

} // namespace examples::model
