#pragma once

#include "model/note.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/db_client.h"
#include <vector>
#include <string>
#include <memory>

using namespace novaboot::annotations;
using todo_notes::model::Note;

struct [[= Repository() ]] NoteRepository : public novaboot::db::CrudRepository<Note, int> {
public:
    explicit NoteRepository(std::shared_ptr<novaboot::db::DataSource> ds)
        : novaboot::db::CrudRepository<Note, int>(ds) {}

    std::vector<Note> find_by_user_id(const std::string& user_id) {
        return query()
            .where<&Note::user_id>(novaboot::db::Op::Equal, user_id)
            .list();
    }
};
