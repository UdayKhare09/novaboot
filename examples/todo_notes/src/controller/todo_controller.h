#pragma once
#include "novaboot/context/request_context.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "service/todo_service.h"
#include "model/dto.h"
#include "model/todo.h"
#include <vector>

using namespace novaboot;
using namespace novaboot::context;
using namespace novaboot::middleware;
using todo_notes::service::TodoService;
using todo_notes::model::TodoRequest;
using todo_notes::model::Todo;

namespace todo_notes::controller {

struct TodoController {
    TodoService& todo_service;

    explicit TodoController(TodoService& svc) : todo_service(svc) {}

    std::string get_user_id(RequestContext& ctx) {
        auto principal = ctx.get<JwtPrincipal>();
        if (!principal) {
            throw std::runtime_error("Unauthorized access");
        }
        auto uid = principal->claims.string("user_id");
        if (!uid) {
            throw std::runtime_error("User ID not found in JWT");
        }
        return std::string(*uid);
    }

    ResponseEntity<std::vector<Todo>> list_todos(RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        return ResponseEntity<std::vector<Todo>>::ok(todo_service.get_todos(uid));
    }

    ResponseEntity<Todo> get_todo(int id, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        auto items = todo_service.get_todos(uid);
        for (const auto& item : items) {
            if (item.id == id) {
                return ResponseEntity<Todo>::ok(item);
            }
        }
        throw std::runtime_error("Todo not found or access denied");
    }

    ResponseEntity<Todo> create_todo(TodoRequest req, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        auto saved = todo_service.create_todo(uid, req);
        return ResponseEntity<Todo>::status(201, saved);
    }

    ResponseEntity<Todo> update_todo(int id, TodoRequest req, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        auto saved = todo_service.update_todo(uid, id, req);
        return ResponseEntity<Todo>::ok(saved);
    }

    ResponseEntity<void> delete_todo(int id, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        todo_service.delete_todo(uid, id);
        return ResponseEntity<void>::noContent();
    }
};

} // namespace todo_notes::controller
