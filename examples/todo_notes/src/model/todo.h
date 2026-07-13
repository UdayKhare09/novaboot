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

#pragma db object table("todos")
struct Todo {
#pragma db id auto
    int id = 0;
    int user_id = 0;
    std::string title;
    std::string description;
    bool completed = false;

#ifndef ODB_COMPILER
    inline static const Schema<Todo> validator =
        Schema<Todo>()
            .field<&Todo::title>("title").not_empty().size(1, 100);
#endif
};

} // namespace todo_notes::model
