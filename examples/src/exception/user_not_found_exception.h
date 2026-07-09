#pragma once
#include <stdexcept>
#include <string>

namespace examples::exception {

class UserNotFoundException : public std::runtime_error {
public:
    explicit UserNotFoundException(int id)
        : std::runtime_error("User with ID " + std::to_string(id) + " not found"), id_(id) {}
        
    int id() const noexcept { return id_; }

private:
    int id_;
};

} // namespace examples::exception
