#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <concepts>
#ifdef __cpp_impl_reflection
#  include <meta>
#endif

namespace novaboot::validation {

struct size {
    std::size_t min = 0;
    std::size_t max = -1;
};

struct email {};

struct min {
    long long value;
};

struct max {
    long long value;
};

struct not_empty {};

class ValidationException : public std::runtime_error {
public:
    explicit ValidationException(std::vector<std::string> errors)
        : std::runtime_error("Validation failed"), errors_(std::move(errors)) {}

    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

private:
    std::vector<std::string> errors_;
};

template<typename A, typename V>
concept is_custom_validator = requires(A a, V v, std::string& err) {
    { a.validate(v, err) } -> std::same_as<bool>;
};

#ifdef __cpp_impl_reflection
template<typename T>
consteval auto get_members() {
    constexpr auto ctx = std::meta::access_context::current();
    struct ArrayWrapper {
        std::meta::info data[64] = {};
        std::size_t     size = 0;

        consteval const std::meta::info* begin() const noexcept { return data; }
        consteval const std::meta::info* end() const noexcept { return data + size; }
        consteval std::meta::info operator[](std::size_t idx) const noexcept { return data[idx]; }
    };
    ArrayWrapper result;
    for (auto m : std::meta::members_of(^^T, ctx)) {
        if (result.size < 64) {
            result.data[result.size++] = m;
        }
    }
    return result;
}

template<std::meta::info m>
consteval auto get_annotations() {
    struct AnnArrayWrapper {
        std::meta::info data[16] = {};
        std::size_t     size = 0;

        consteval const std::meta::info* begin() const noexcept { return data; }
        consteval const std::meta::info* end() const noexcept { return data + size; }
        consteval std::meta::info operator[](std::size_t idx) const noexcept { return data[idx]; }
    };
    AnnArrayWrapper result;
    for (auto ann : std::meta::annotations_of(m)) {
        if (result.size < 16) {
            result.data[result.size++] = ann;
        }
    }
    return result;
}
#endif

template<typename T>
bool validate([[maybe_unused]] const T& obj, [[maybe_unused]] std::vector<std::string>& errors) {
#ifdef __cpp_impl_reflection
    static constexpr auto members = get_members<T>();
    bool valid = true;
    template for (constexpr auto m : members) {
        if constexpr (std::meta::is_nonstatic_data_member(m)) {
            constexpr auto name = std::meta::identifier_of(m);
            const auto& val = obj.*&[:m:];

            // 1. size constraint
            if constexpr (!std::meta::annotations_of_with_type(m, ^^size).empty()) {
                constexpr auto ann = std::meta::annotations_of_with_type(m, ^^size)[0];
                constexpr auto attr = std::meta::extract<size>(ann);
                if (val.size() < attr.min || val.size() > attr.max) {
                    errors.push_back(std::string(name) + " length must be between " + std::to_string(attr.min) + " and " + std::to_string(attr.max));
                    valid = false;
                }
            }

            // 2. not_empty constraint
            if constexpr (!std::meta::annotations_of_with_type(m, ^^not_empty).empty()) {
                if (val.empty()) {
                    errors.push_back(std::string(name) + " must not be empty");
                    valid = false;
                }
            }

            // 3. min constraint
            if constexpr (!std::meta::annotations_of_with_type(m, ^^min).empty()) {
                constexpr auto ann = std::meta::annotations_of_with_type(m, ^^min)[0];
                constexpr auto attr = std::meta::extract<min>(ann);
                if (val < attr.value) {
                    errors.push_back(std::string(name) + " must be at least " + std::to_string(attr.value));
                    valid = false;
                }
            }

            // 4. max constraint
            if constexpr (!std::meta::annotations_of_with_type(m, ^^max).empty()) {
                constexpr auto ann = std::meta::annotations_of_with_type(m, ^^max)[0];
                constexpr auto attr = std::meta::extract<max>(ann);
                if (val > attr.value) {
                    errors.push_back(std::string(name) + " must be at most " + std::to_string(attr.value));
                    valid = false;
                }
            }

            // 5. email constraint
            if constexpr (!std::meta::annotations_of_with_type(m, ^^email).empty()) {
                if (val.find('@') == std::string::npos) {
                    errors.push_back(std::string(name) + " must be a valid email address");
                    valid = false;
                }
            }

            // 6. Custom validators
            template for (constexpr auto ann : get_annotations<m>()) {
                constexpr auto ann_type = std::meta::type_of(ann);
                using AnnType = typename[: ann_type :];
                if constexpr (is_custom_validator<AnnType, decltype(val)>) {
                    constexpr auto ann_val = std::meta::extract<AnnType>(ann);
                    AnnType validator = ann_val;
                    std::string custom_err;
                    if (!validator.validate(val, custom_err)) {
                        errors.push_back(std::string(name) + " " + custom_err);
                        valid = false;
                    }
                }
            }
        }
    }
    return valid;
#else
    return true;
#endif
}

} // namespace novaboot::validation
