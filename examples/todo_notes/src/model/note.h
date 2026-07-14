#pragma once
#include <string>
#include "novaboot/validation/validation.h"

namespace todo_notes::model {

using novaboot::validation::Schema;

struct Note {
    int id = 0;
    std::string user_id;
    std::string title;
    std::string content;

    inline static const Schema<Note> validator =
        Schema<Note>()
            .field<&Note::title>("title").not_empty().size(1, 100);
};

} // namespace todo_notes::model
