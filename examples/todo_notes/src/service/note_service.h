#pragma once
#include "repository/note_repository.h"
#include "model/dto.h"
#include <stdexcept>
#include <vector>

#include "novaboot/novaboot.h"

namespace todo_notes::service {

using namespace novaboot::annotations;
using todo_notes::model::Note;
using todo_notes::model::NoteRequest;

struct [[= Service() ]] NoteService {
    NoteRepository& note_repo;

    explicit NoteService(NoteRepository& repo) : note_repo(repo) {}

    std::vector<Note> get_notes(const std::string& user_id) {
        return note_repo.find_by_user_id(user_id);
    }

    Note create_note(const std::string& user_id, const NoteRequest& req) {
        Note note;
        note.id = "";
        note.user_id = user_id;
        note.title = req.title;
        note.content = req.content;

        return note_repo.save(note);
    }

    Note update_note(const std::string& user_id, const std::string& id, const NoteRequest& req) {
        auto existing = note_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Note not found or access denied");
        }

        existing->title = req.title;
        existing->content = req.content;

        return note_repo.save(*existing);
    }

    void delete_note(const std::string& user_id, const std::string& id) {
        auto existing = note_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Note not found or access denied");
        }

        note_repo.delete_by_id(id);
    }
};

} // namespace todo_notes::service
