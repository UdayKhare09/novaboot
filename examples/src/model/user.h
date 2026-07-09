#pragma once
#include <string>

namespace examples::model {

struct User {
    int id;
    std::string name;
    std::string email;
    std::string role;
};

} // namespace examples::model
