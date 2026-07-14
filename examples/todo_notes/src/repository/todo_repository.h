#pragma once

#include "model/todo.h"
#include <vector>
#include <optional>
#include <mutex>
#include <algorithm>

using todo_notes::model::Todo;

struct TodoRepository {
private:
    std::vector<Todo> todos_;
    std::mutex mutex_;
    int next_id_ = 1;

public:
    TodoRepository() = default;

    std::vector<Todo> find_by_user_id(const std::string& user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Todo> result;
        for (const auto& t : todos_) {
            if (t.user_id == user_id) {
                result.push_back(t);
            }
        }
        return result;
    }

    std::optional<Todo> find_by_id(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(todos_.begin(), todos_.end(), [id](const Todo& t) { return t.id == id; });
        if (it != todos_.end()) return *it;
        return std::nullopt;
    }

    Todo save(Todo todo) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (todo.id == 0) {
            todo.id = next_id_++;
            todos_.push_back(todo);
            return todo;
        } else {
            auto it = std::find_if(todos_.begin(), todos_.end(), [id = todo.id](const Todo& t) { return t.id == id; });
            if (it != todos_.end()) {
                *it = todo;
                return todo;
            } else {
                todos_.push_back(todo);
                return todo;
            }
        }
    }

    void delete_by_id(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        todos_.erase(
            std::remove_if(todos_.begin(), todos_.end(), [id](const Todo& t) { return t.id == id; }),
            todos_.end()
        );
    }
};
