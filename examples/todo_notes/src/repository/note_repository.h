#pragma once

#include "model/note.h"
#include <vector>
#include <optional>
#include <mutex>
#include <algorithm>

#include "novaboot/novaboot.h"

using namespace novaboot::annotations;
using todo_notes::model::Note;

struct [[= Repository() ]] NoteRepository {
private:
    std::vector<Note> notes_;
    std::mutex mutex_;
    int next_id_ = 1;

public:
    NoteRepository() = default;

    std::vector<Note> find_by_user_id(const std::string& user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Note> result;
        for (const auto& n : notes_) {
            if (n.user_id == user_id) {
                result.push_back(n);
            }
        }
        return result;
    }

    std::optional<Note> find_by_id(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(), [id](const Note& n) { return n.id == id; });
        if (it != notes_.end()) return *it;
        return std::nullopt;
    }

    Note save(Note note) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (note.id == 0) {
            note.id = next_id_++;
            notes_.push_back(note);
            return note;
        } else {
            auto it = std::find_if(notes_.begin(), notes_.end(), [id = note.id](const Note& n) { return n.id == id; });
            if (it != notes_.end()) {
                *it = note;
                return note;
            } else {
                notes_.push_back(note);
                return note;
            }
        }
    }

    void delete_by_id(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        notes_.erase(
            std::remove_if(notes_.begin(), notes_.end(), [id](const Note& n) { return n.id == id; }),
            notes_.end()
        );
    }
};
