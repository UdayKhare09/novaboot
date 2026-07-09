#pragma once
#include <string>
#include "novaboot/validation/validation.h"
#include "novaboot/lombok/lombok.h"

namespace examples::model {

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

struct [[=lombok::data{}]] [[=lombok::builder{}]] User {
    [[=novaboot::validation::min{0}]]
    int id;

    [[=novaboot::validation::not_empty{}]]
    [[=novaboot::validation::size{.min = 2, .max = 20}]]
    std::string name;

    [[=novaboot::validation::email{}]]
    std::string email;

    [[=examples::model::is_valid_role{}]]
    std::string role;

    #include "user.lombok.h"
};

} // namespace examples::model
