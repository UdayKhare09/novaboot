#pragma once

#include "model/todo.h"
#include "novaboot/db/repository.h"
#include "novaboot/db/db_client.h"
#include <vector>
#include <string>
#include <memory>

using namespace novaboot::annotations;
using todo_notes::model::Todo;

struct [[= Repository() ]] TodoRepository : public novaboot::db::CrudRepository<Todo, int> {
public:
    explicit TodoRepository(std::shared_ptr<novaboot::db::DataSource> ds)
        : novaboot::db::CrudRepository<Todo, int>(ds) {}

    std::vector<Todo> find_by_user_id(const std::string& user_id) {
        return query()
            .where<&Todo::user_id>(novaboot::db::Op::Equal, user_id)
            .list();
    }
};
