#pragma once
#include "novaboot/context/request_context.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "service/note_service.h"
#include "model/dto.h"
#include "model/note.h"
#include <vector>

using namespace novaboot;
using namespace novaboot::context;
using namespace novaboot::middleware;
using todo_notes::service::NoteService;
using todo_notes::model::NoteRequest;
using todo_notes::model::Note;

using namespace novaboot::annotations;

namespace todo_notes::controller {

struct [[= RestController("/api/notes") ]] NoteController {
    NoteService& note_service;

    explicit NoteController(NoteService& svc) : note_service(svc) {}

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

    [[= GetMapping("") ]]
    ResponseEntity<std::vector<Note>> list_notes(RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        return ResponseEntity<std::vector<Note>>::ok(note_service.get_notes(uid));
    }

    [[= GetMapping("/:id") ]]
    ResponseEntity<Note> get_note(int id, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        auto items = note_service.get_notes(uid);
        for (const auto& item : items) {
            if (item.id == id) {
                return ResponseEntity<Note>::ok(item);
            }
        }
        throw std::runtime_error("Note not found or access denied");
    }

    [[= PostMapping("") ]]
    ResponseEntity<Note> create_note(NoteRequest req, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        auto saved = note_service.create_note(uid, req);
        return ResponseEntity<Note>::status(201, saved);
    }

    [[= PutMapping("/:id") ]]
    ResponseEntity<Note> update_note(int id, NoteRequest req, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        auto saved = note_service.update_note(uid, id, req);
        return ResponseEntity<Note>::ok(saved);
    }

    [[= DeleteMapping("/:id") ]]
    ResponseEntity<void> delete_note(int id, RequestContext& ctx) {
        std::string uid = get_user_id(ctx);
        note_service.delete_note(uid, id);
        return ResponseEntity<void>::noContent();
    }
};

} // namespace todo_notes::controller
