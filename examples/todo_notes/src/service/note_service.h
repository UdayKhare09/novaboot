#pragma once
#include "repository/note_repository.h"
#include "model/dto.h"
#include <stdexcept>
#include <vector>

namespace todo_notes::service {

using todo_notes::model::Note;
using todo_notes::model::NoteRequest;

struct NoteService {
    NoteRepository& note_repo;

    explicit NoteService(NoteRepository& repo) : note_repo(repo) {}

    std::vector<Note> get_notes(int user_id) {
        return note_repo.find_by_user_id(user_id);
    }

    Note create_note(int user_id, const NoteRequest& req) {
        Note note;
        note.id = 0;
        note.user_id = user_id;
        note.title = req.title;
        note.content = req.content;

        return note_repo.save(note);
    }

    Note update_note(int user_id, int id, const NoteRequest& req) {
        auto existing = note_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Note not found or access denied");
        }

        existing->title = req.title;
        existing->content = req.content;

        return note_repo.save(*existing);
    }

    void delete_note(int user_id, int id) {
        auto existing = note_repo.find_by_id(id);
        if (!existing || existing->user_id != user_id) {
            throw std::runtime_error("Note not found or access denied");
        }

        note_repo.delete_by_id(id);
    }
};

} // namespace todo_notes::service
