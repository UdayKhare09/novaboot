#pragma once

#include "novaboot/di/di.h"
#include "novaboot/router/web_attributes.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/context/request_context.h"
#include "exception/user_not_found_exception.h"
#include <string>

struct ErrorResponse {
    std::string error;
    std::string message;
};

struct [[=novaboot::web::controller_advice{}]] GlobalExceptionHandler {
    GlobalExceptionHandler() = default;

    [[=novaboot::web::exception_handler{^^examples::exception::UserNotFoundException}]]
    auto handle_user_not_found(const examples::exception::UserNotFoundException& ex, novaboot::context::RequestContext&) {
        ErrorResponse err{"User Not Found", ex.what()};
        return novaboot::ResponseEntity<ErrorResponse>::status(404, err);
    }
};
