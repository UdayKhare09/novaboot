#pragma once
#include "novaboot/di/scope.h"

namespace novaboot::annotations {

/// Mark a struct/class as a generic component managed by DI container.
struct Component {
    di::Scope scope = di::Scope::Singleton;
    consteval Component() = default;
    consteval explicit Component(di::Scope s) : scope(s) {}
};

/// Mark a struct/class as a business logic service component managed by DI container.
struct Service {
    di::Scope scope = di::Scope::Singleton;
    consteval Service() = default;
    consteval explicit Service(di::Scope s) : scope(s) {}
};

/// Mark a struct/class as a data access repository component managed by DI container.
struct Repository {
    di::Scope scope = di::Scope::Singleton;
    consteval Repository() = default;
    consteval explicit Repository(di::Scope s) : scope(s) {}
};

/// Mark a struct/class as a REST Controller mapping HTTP routes.
struct RestController {
    char base_path[64] = {};
    consteval RestController() = default;
    consteval explicit RestController(const char* path) {
        int i = 0;
        while (path[i] && i < 63) {
            base_path[i] = path[i];
            i++;
        }
        base_path[i] = '\0';
    }
};

/// Mark a struct/class as a global controller advice for handling exceptions.
struct ControllerAdvice {
    consteval ControllerAdvice() = default;
};

/// Mark a controller advice method as an exception handler.
struct ExceptionHandler {
    consteval ExceptionHandler() = default;
};

} // namespace novaboot::annotations
