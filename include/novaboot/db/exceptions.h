#pragma once
#include <stdexcept>
#include <string>

namespace novaboot::db {

class DatabaseException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ConstraintViolationException : public DatabaseException {
public:
    explicit ConstraintViolationException(const std::string& message)
        : DatabaseException(message) {}
};

class UniqueConstraintViolationException : public ConstraintViolationException {
public:
    explicit UniqueConstraintViolationException(const std::string& message)
        : ConstraintViolationException(message) {}
};

class ForeignKeyConstraintViolationException : public ConstraintViolationException {
public:
    explicit ForeignKeyConstraintViolationException(const std::string& message)
        : ConstraintViolationException(message) {}
};

class NotNullConstraintViolationException : public ConstraintViolationException {
public:
    explicit NotNullConstraintViolationException(const std::string& message)
        : ConstraintViolationException(message) {}
};

/// Thrown by CrudRepository::save() when an optimistic-locking conflict is detected.
///
/// This happens when two concurrent callers both load the same entity, modify it,
/// and then try to save it back. The second save will see that the database version
/// no longer matches the in-memory version and raises this exception.
///
/// Typical response: return HTTP 409 Conflict. Wire via @ExceptionHandler.
class OptimisticLockException : public std::runtime_error {
public:
    OptimisticLockException()
        : std::runtime_error(
              "Optimistic lock conflict: the entity was modified or deleted "
              "by another transaction. Reload and retry.")
    {}

    explicit OptimisticLockException(const std::string& table)
        : std::runtime_error(
              "Optimistic lock conflict on table '" + table +
              "': entity was modified concurrently. Reload and retry.")
    {}
};

} // namespace novaboot::db
