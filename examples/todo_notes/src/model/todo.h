#pragma once
#include <string>
#include "novaboot/validation/validation.h"

namespace todo_notes::model {

using novaboot::validation::Schema;

struct Todo {
    int id = 0;
    std::string user_id;
    std::string title;
    std::string description;
    bool completed = false;

    inline static const Schema<Todo> validator =
        Schema<Todo>()
            .field<&Todo::title>("title").not_empty().size(1, 100);
};

} // namespace todo_notes::model
