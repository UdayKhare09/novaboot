#pragma once

#include "model/dto.h"
#include "novaboot/db/exceptions.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/validation/validation.h"

#include <stdexcept>

namespace knowledge_hub::controller {

using knowledge_hub::model::ErrorResponse;
using namespace novaboot::annotations;

struct [[= ControllerAdvice() ]] GlobalExceptionHandler {
    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_validation(
        const novaboot::validation::ValidationException& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            422,
            ErrorResponse{
                .status = 422,
                .error = "Validation Failed",
                .message = error.what(),
                .details = error.errors(),
            });
    }

    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_conflict(
        const novaboot::db::OptimisticLockException& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            409,
            ErrorResponse{
                .status = 409,
                .error = "Conflict",
                .message = error.what(),
                .details = {},
            });
    }

    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_unique_constraint(
        const novaboot::db::UniqueConstraintViolationException& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            409,
            ErrorResponse{
                .status = 409,
                .error = "Conflict",
                .message = error.what(),
                .details = {},
            });
    }

    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_constraint(
        const novaboot::db::ConstraintViolationException& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            400,
            ErrorResponse{
                .status = 400,
                .error = "Constraint Violation",
                .message = error.what(),
                .details = {},
            });
    }

    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_invalid_argument(
        const std::invalid_argument& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            400,
            ErrorResponse{
                .status = 400,
                .error = "Bad Request",
                .message = error.what(),
                .details = {},
            });
    }

    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_runtime(const std::runtime_error& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            400,
            ErrorResponse{
                .status = 400,
                .error = "Bad Request",
                .message = error.what(),
                .details = {},
            });
    }

    [[= ExceptionHandler() ]]
    novaboot::ResponseEntity<ErrorResponse> handle_exception(const std::exception& error) {
        return novaboot::ResponseEntity<ErrorResponse>::status(
            500,
            ErrorResponse{
                .status = 500,
                .error = "Server Error",
                .message = error.what(),
                .details = {},
            });
    }
};

} // namespace knowledge_hub::controller
