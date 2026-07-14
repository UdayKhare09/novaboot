#pragma once
#include "novaboot/context/request_context.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/validation/validation.h"
#include "model/dto.h"
#include <stdexcept>
#include <string>

using namespace novaboot;
using namespace novaboot::annotations;
using namespace novaboot::context;
using todo_notes::model::ErrorResponse;

namespace todo_notes::controller {

struct [[= ControllerAdvice() ]] GlobalExceptionHandler {
    [[= ExceptionHandler() ]]
    ResponseEntity<ErrorResponse> handle_runtime(const std::runtime_error& ex) {
        ErrorResponse err{"Bad Request", ex.what()};
        return ResponseEntity<ErrorResponse>::status(400, err);
    }

    [[= ExceptionHandler() ]]
    ResponseEntity<ErrorResponse> handle_validation(const novaboot::validation::ValidationException& ex) {
        std::string msg;
        for (const auto& err : ex.errors()) {
            if (!msg.empty()) msg += ", ";
            msg += err;
        }
        ErrorResponse err{"Validation Failed", msg};
        return ResponseEntity<ErrorResponse>::status(400, err);
    }
};

} // namespace todo_notes::controller
