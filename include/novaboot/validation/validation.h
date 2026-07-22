#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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
public:
    /// Reflection-independent description retained alongside runtime
    /// validators. OpenAPI can consume this without invoking validators or
    /// inspecting erased route-handler types.
    struct FieldMetadata {
        std::string name;
        std::string json_type;
        std::optional<std::string> item_json_type;
        std::optional<long long> minimum;
        std::optional<long long> maximum;
        std::optional<std::size_t> min_length;
        std::optional<std::size_t> max_length;
        std::optional<std::string> format;
        bool required = false;
    };

private:
    template<typename Value>
    struct is_vector : std::false_type {};
    template<typename Value, typename Allocator>
    struct is_vector<std::vector<Value, Allocator>> : std::true_type {
        using value_type = Value;
    };

    using ValidatorFn = std::function<bool(const T&, std::vector<std::string>&)>;
    std::vector<ValidatorFn> validators_;
    std::vector<FieldMetadata> fields_;

    template<typename Value>
    static std::string json_type() {
        using Field = std::remove_cvref_t<Value>;
        if constexpr (std::same_as<Field, std::string> || std::same_as<Field, std::string_view> ||
                      std::same_as<Field, const char*>) {
            return "string";
        } else if constexpr (std::same_as<Field, bool>) {
            return "boolean";
        } else if constexpr (std::integral<Field>) {
            return "integer";
        } else if constexpr (std::floating_point<Field>) {
            return "number";
        } else if constexpr (is_vector<Field>::value) {
            return "array";
        } else {
            return "object";
        }
    }

    template<typename Value>
    static std::optional<std::string> item_json_type() {
        using Field = std::remove_cvref_t<Value>;
        if constexpr (is_vector<Field>::value) {
            return json_type<typename is_vector<Field>::value_type>();
        }
        return std::nullopt;
    }

    template<auto MemberPtr>
    std::size_t add_field(std::string name) {
        using Value = std::remove_cvref_t<decltype(std::declval<T>().*MemberPtr)>;
        FieldMetadata field;
        field.name = std::move(name);
        field.json_type = json_type<Value>();
        field.item_json_type = item_json_type<Value>();
        fields_.push_back(std::move(field));
        return fields_.size() - 1U;
    }

public:
    template<auto MemberPtr>
    class FieldBuilder {
    public:
        FieldBuilder(Schema& schema, std::string name)
            : schema_(schema), name_(std::move(name)), field_index_(schema_.template add_field<MemberPtr>(name_)) {}

        FieldBuilder& min(long long value) {
            schema_.fields_[field_index_].minimum = value;
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
            schema_.fields_[field_index_].maximum = value;
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
            schema_.fields_[field_index_].required = true;
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
            schema_.fields_[field_index_].min_length = minimum;
            schema_.fields_[field_index_].max_length = maximum;
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
            schema_.fields_[field_index_].format = "email";
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
        std::size_t field_index_ = 0;
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

    [[nodiscard]] const std::vector<FieldMetadata>& fields() const noexcept {
        return fields_;
    }
};

template<typename T>
bool validate(const T& object, std::vector<std::string>& errors) {
    static_assert(requires { T::validator.validate(object, errors); },
                  "Define T::validator as novaboot::validation::Schema<T>.");
    return T::validator.validate(object, errors);
}

} // namespace novaboot::validation
