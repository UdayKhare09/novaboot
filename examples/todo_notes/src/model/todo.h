#pragma once
#include <string>
#include "novaboot/validation/validation.h"
#include "novaboot/annotations/stereotypes.h"

namespace todo_notes::model {

using novaboot::validation::Schema;
using namespace novaboot::annotations;

struct [[= Entity("todos") ]] Todo {
    [[= Id() ]]
    [[= GeneratedValue(GenerationType::UUID) ]]
    std::string id;
    
    std::string user_id;
    std::string title;
    std::string description;
    bool completed = false;

    inline static const Schema<Todo> validator =
        Schema<Todo>()
            .field<&Todo::title>("title").not_empty().size(1, 100);
};

} // namespace todo_notes::model
