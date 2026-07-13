#pragma once
#include "repository/todo_repository.h"
#include "model/dto.h"
#include <stdexcept>
#include <vector>

namespace todo_notes::service {

using todo_notes::model::Todo;
using todo_notes::model::TodoRequest;

struct TodoService {
    TodoRepository& todo_repo;

    explicit TodoService(TodoRepository& repo) : todo_repo(repo) {}

    std::vector<Todo> get_todos(int user_id) {
        return todo_repo.find_by_user_id(user_id);
    }

    Todo create_todo(int user_id, const TodoRequest& req) {
        Todo todo;
        todo.id = 0;
        todo.user_id = user_id;
        todo.title = req.title;
        todo.description = req.description;
        todo.completed = req.completed;

        return todo_repo.save(todo);
    }

    Todo update_todo(int user_id, int id, const TodoRequest& req) {
        auto existing = todo_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Todo not found or access denied");
        }

        existing->title = req.title;
        existing->description = req.description;
        existing->completed = req.completed;

        return todo_repo.save(*existing);
    }

    void delete_todo(int user_id, int id) {
        auto existing = todo_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Todo not found or access denied");
        }

        todo_repo.delete_by_id(id);
    }
};

} // namespace todo_notes::service
