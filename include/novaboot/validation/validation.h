#pragma once

#include <concepts>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace novaboot::validation {

class ValidationException : public std::runtime_error {
public:
    explicit ValidationException(std::vector<std::string> errors)
        : std::runtime_error("Validation failed"), errors_(std::move(errors)) {}

    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

private:
    std::vector<std::string> errors_;
};

template<typename T>
class Schema {
    using ValidatorFn = std::function<bool(const T&, std::vector<std::string>&)>;
    std::vector<ValidatorFn> validators_;

public:
    template<auto MemberPtr>
    class FieldBuilder {
    public:
        FieldBuilder(Schema& schema, std::string name)
            : schema_(schema), name_(std::move(name)) {}

        FieldBuilder& min(long long value) {
            schema_.validators_.push_back([name = name_, value](const T& object, auto& errors) {
                if ((object.*MemberPtr) < value) {
                    errors.push_back(name + " must be at least " + std::to_string(value));
                    return false;
                }
                return true;
            });
            return *this;
        }

        FieldBuilder& max(long long value) {
            schema_.validators_.push_back([name = name_, value](const T& object, auto& errors) {
                if ((object.*MemberPtr) > value) {
                    errors.push_back(name + " must be at most " + std::to_string(value));
                    return false;
                }
                return true;
            });
            return *this;
        }

        FieldBuilder& not_empty() {
            schema_.validators_.push_back([name = name_](const T& object, auto& errors) {
                if ((object.*MemberPtr).empty()) {
                    errors.push_back(name + " must not be empty");
                    return false;
                }
                return true;
            });
            return *this;
        }

        FieldBuilder& size(std::size_t minimum, std::size_t maximum) {
            schema_.validators_.push_back([name = name_, minimum, maximum](const T& object, auto& errors) {
                const auto length = (object.*MemberPtr).size();
                if (length < minimum || length > maximum) {
                    errors.push_back(name + " length must be between " + std::to_string(minimum)
                                     + " and " + std::to_string(maximum));
                    return false;
                }
                return true;
            });
            return *this;
        }

        FieldBuilder& email() {
            schema_.validators_.push_back([name = name_](const T& object, auto& errors) {
                if ((object.*MemberPtr).find('@') == std::string::npos) {
                    errors.push_back(name + " must be a valid email address");
                    return false;
                }
                return true;
            });
            return *this;
        }

        template<typename Validator>
        FieldBuilder& custom(Validator validator) {
            schema_.validators_.push_back([name = name_, validator = std::move(validator)](const T& object, auto& errors) {
                std::string error;
                if (!validator.validate(object.*MemberPtr, error)) {
                    errors.push_back(name + " " + error);
                    return false;
                }
                return true;
            });
            return *this;
        }

        template<auto NextMemberPtr>
        FieldBuilder<NextMemberPtr> field(std::string name) {
            return schema_.template field<NextMemberPtr>(std::move(name));
        }

        operator Schema() const { return schema_; }

    private:
        Schema& schema_;
        std::string name_;
    };

    template<auto MemberPtr>
    FieldBuilder<MemberPtr> field(std::string name) {
        return FieldBuilder<MemberPtr>(*this, std::move(name));
    }

    bool validate(const T& object, std::vector<std::string>& errors) const {
        bool valid = true;
        for (const auto& validator : validators_) valid = validator(object, errors) && valid;
        return valid;
    }
};

template<typename T>
bool validate(const T& object, std::vector<std::string>& errors) {
    static_assert(requires { T::validator.validate(object, errors); },
                  "Define T::validator as novaboot::validation::Schema<T>.");
    return T::validator.validate(object, errors);
}

} // namespace novaboot::validation
