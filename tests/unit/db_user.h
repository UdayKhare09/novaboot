#pragma once

#include <string>
#include "novaboot/data/data_attributes.h"

struct [[=novaboot::data::entity{"db_users"}]] DbUser {
    [[=novaboot::data::id{}]]
    int id = 0;

    std::string name;
    std::string email;
};
