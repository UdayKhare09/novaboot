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

/// Mark a class/struct as a configuration class containing factory methods.
struct Configuration {
    consteval Configuration() = default;
};

/// Mark a configuration class method as a bean factory method.
struct Bean {
    di::Scope scope = di::Scope::Singleton;
    consteval Bean() = default;
    consteval explicit Bean(di::Scope s) : scope(s) {}
};

/// Specify execution order for components like filters/middlewares.
struct Order {
    int value = 0;
    consteval Order() = default;
    consteval Order(int val) : value(val) {}
};

/// Mark a struct/class as a database entity
struct Entity {
    char name[64] = {};
    consteval Entity() = default;
    consteval Entity(const char* n) {
        int i = 0;
        while (n[i] && i < 63) {
            name[i] = n[i];
            i++;
        }
        name[i] = '\0';
    }
};

/// Designate the primary key field
struct Id {
    consteval Id() = default;
};

enum class GenerationType {
    AutoIncrement,
    UUID
};

/// Mark field for auto-increment identity column
struct GeneratedValue {
    GenerationType strategy = GenerationType::AutoIncrement;
    consteval GeneratedValue() = default;
    consteval GeneratedValue(GenerationType s) : strategy(s) {}
};

/// Field-level column customization
struct Column {
    char name[64] = {};
    bool nullable = true;

    consteval Column() = default;
    consteval Column(const char* col_name, bool is_nullable = true) 
        : nullable(is_nullable) {
        int i = 0;
        while (col_name[i] && i < 63) {
            name[i] = col_name[i];
            i++;
        }
        name[i] = '\0';
    }
};

/// Mark string field to store formatted JSON objects
struct Json {
    consteval Json() = default;
};

} // namespace novaboot::annotations
