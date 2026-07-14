#pragma once
#include "repository/todo_repository.h"
#include "model/dto.h"
#include <stdexcept>
#include <vector>

#include "novaboot/novaboot.h"

namespace todo_notes::service {

using namespace novaboot::annotations;
using todo_notes::model::Todo;
using todo_notes::model::TodoRequest;

struct [[= Service() ]] TodoService {
    TodoRepository& todo_repo;

    explicit TodoService(TodoRepository& repo) : todo_repo(repo) {}

    std::vector<Todo> get_todos(const std::string& user_id) {
        return todo_repo.find_by_user_id(user_id);
    }

    Todo create_todo(const std::string& user_id, const TodoRequest& req) {
        Todo todo;
        todo.id = "";
        todo.user_id = user_id;
        todo.title = req.title;
        todo.description = req.description;
        todo.completed = req.completed;
        todo.priority = req.priority;
        todo.temp_note = "Transient note created";

        return todo_repo.save(todo);
    }

    Todo update_todo(const std::string& user_id, const std::string& id, const TodoRequest& req) {
        auto existing = todo_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Todo not found or access denied");
        }

        existing->title = req.title;
        existing->description = req.description;
        existing->completed = req.completed;
        existing->priority = req.priority;
        existing->temp_note = "Transient note updated";

        return todo_repo.save(*existing);
    }

    void delete_todo(const std::string& user_id, const std::string& id) {
        auto existing = todo_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Todo not found or access denied");
        }

        todo_repo.delete_by_id(id);
    }
};

} // namespace todo_notes::service
