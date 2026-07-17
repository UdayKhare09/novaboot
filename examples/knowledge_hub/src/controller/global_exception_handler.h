#pragma once

#include "model/dto.h"
#include "novaboot/router/response_entity.h"

#include <stdexcept>

namespace knowledge_hub::controller {

using knowledge_hub::model::ErrorResponse;
using namespace novaboot::annotations;

struct [[= ControllerAdvice() ]] GlobalExceptionHandler {
    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_runtime(const std::runtime_error& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            400, ErrorResponse{.error = "Bad Request", .message = error.what()});
    }

    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_exception(const std::exception& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            500, ErrorResponse{.error = "Server Error", .message = error.what()});
    }
};

} // namespace knowledge_hub::controller
