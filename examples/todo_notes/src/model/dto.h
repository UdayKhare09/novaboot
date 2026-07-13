#pragma once
#include <string>
#include "novaboot/validation/validation.h"

namespace todo_notes::model {

using novaboot::validation::Schema;

struct ErrorResponse {
    std::string error;
    std::string message;
};

struct RegisterRequest {
    std::string username;
    std::string password;
    std::string email;

    inline static const Schema<RegisterRequest> validator =
        Schema<RegisterRequest>()
            .field<&RegisterRequest::username>("username").not_empty().size(3, 30)
            .field<&RegisterRequest::password>("password").not_empty().size(6, 50)
            .field<&RegisterRequest::email>("email").email();
};

struct LoginRequest {
    std::string username;
    std::string password;

    inline static const Schema<LoginRequest> validator =
        Schema<LoginRequest>()
            .field<&LoginRequest::username>("username").not_empty()
            .field<&LoginRequest::password>("password").not_empty();
};

struct LoginResponse {
    std::string token;
};

struct TodoRequest {
    std::string title;
    std::string description;
    bool completed = false;

    inline static const Schema<TodoRequest> validator =
        Schema<TodoRequest>()
            .field<&TodoRequest::title>("title").not_empty().size(1, 100);
};

struct NoteRequest {
    std::string title;
    std::string content;

    inline static const Schema<NoteRequest> validator =
        Schema<NoteRequest>()
            .field<&NoteRequest::title>("title").not_empty().size(1, 100);
};

} // namespace todo_notes::model
