#pragma once

#include "novaboot/data/pgsql/pgsql_repository_base.h"
#include "model/todo.h"
#include <odb/query.hxx>
#include "todo-odb.hxx"
#include <vector>

using namespace novaboot;
using namespace novaboot::data;
using todo_notes::model::Todo;

struct TodoRepository : public PgsqlRepositoryBase<Todo, int> {
public:
    explicit TodoRepository(PgsqlDataSource& ds)
        : PgsqlRepositoryBase<Todo, int>(ds) {}

    std::vector<Todo> find_by_user_id(const std::string& user_id) {
        return ds_.transact([&](auto& db) {
            std::vector<Todo> result;
            typedef odb::query<Todo> query;
            auto r = db.template query<Todo>(query::user_id == user_id);
            for (auto& item : r) {
                result.push_back(item);
            }
            return result;
        });
    }
};
