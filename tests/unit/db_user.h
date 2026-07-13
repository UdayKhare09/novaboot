#pragma once

#include <string>
#ifdef ODB_COMPILER
#include <odb/core.hxx>
#endif

#pragma db object table("db_users")
struct DbUser {
#pragma db id
    int id = 0;

    std::string name;
    std::string email;
};
