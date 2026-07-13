#pragma once

#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "model/note.h"
#include <odb/query.hxx>
#include "note-odb.hxx"
#include <vector>

using namespace novaboot;
using namespace novaboot::data;
using todo_notes::model::Note;

struct NoteRepository : public PgsqlRepositoryBase<Note, int> {
public:
    explicit NoteRepository(PgsqlDataSource& ds)
        : PgsqlRepositoryBase<Note, int>(ds) {}

    std::vector<Note> find_by_user_id(const std::string& user_id) {
        return ds_.transact([&](auto& db) {
            std::vector<Note> result;
            typedef odb::query<Note> query;
            auto r = db.template query<Note>(query::user_id == user_id);
            for (auto& item : r) {
                result.push_back(item);
            }
            return result;
        });
    }
};
