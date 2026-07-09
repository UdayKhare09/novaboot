#include <gtest/gtest.h>
#include "novaboot/validation/validation.h"
#include <vector>
#include <string>

struct ValidatedDto {
    [[=novaboot::validation::min{10}]]
    int id;

    [[=novaboot::validation::not_empty{}]]
    [[=novaboot::validation::size{.min = 3, .max = 8}]]
    std::string code;

    [[=novaboot::validation::email{}]]
    std::string email;
};

TEST(ValidationTest, ValidPayloadPasses) {
    ValidatedDto dto{15, "abc", "test@example.com"};
    std::vector<std::string> errors;
    bool success = novaboot::validation::validate(dto, errors);
    EXPECT_TRUE(success);
    EXPECT_EQ(errors.size(), 0);
}

TEST(ValidationTest, InvalidPayloadCollectsAllErrors) {
    ValidatedDto dto{5, "", "invalid-email"};
    std::vector<std::string> errors;
    bool success = novaboot::validation::validate(dto, errors);
    EXPECT_FALSE(success);
    
    // We expect 3 errors:
    // 1. id is less than 10
    // 2. code is empty (violates not_empty and size 3..8)
    // 3. email doesn't have '@'
    EXPECT_GE(errors.size(), 3);
    
    bool found_id_error = false;
    bool found_code_error = false;
    bool found_email_error = false;
    
    for (const auto& err : errors) {
        if (err.find("id") != std::string::npos) found_id_error = true;
        if (err.find("code") != std::string::npos) found_code_error = true;
        if (err.find("email") != std::string::npos) found_email_error = true;
    }
    
    EXPECT_TRUE(found_id_error);
    EXPECT_TRUE(found_code_error);
    EXPECT_TRUE(found_email_error);
}

TEST(ValidationTest, ValidationExceptionCarriesErrors) {
    std::vector<std::string> mock_errors = {"error1", "error2"};
    novaboot::validation::ValidationException ex(mock_errors);
    
    EXPECT_STREQ(ex.what(), "Validation failed");
    EXPECT_EQ(ex.errors().size(), 2);
    EXPECT_EQ(ex.errors()[0], "error1");
    EXPECT_EQ(ex.errors()[1], "error2");
}

struct is_valid_role {
    char prefix[32] = {};
    
    consteval is_valid_role() {
        const char* p = "ROLE_";
        int i = 0;
        while (p[i] && i < 31) {
            prefix[i] = p[i];
            i++;
        }
        prefix[i] = '\0';
    }
    
    consteval is_valid_role(const char* p) {
        int i = 0;
        while (p[i] && i < 31) {
            prefix[i] = p[i];
            i++;
        }
        prefix[i] = '\0';
    }
    
    bool validate(const std::string& val, std::string& err) const {
        std::string pref(prefix);
        if (val.find(pref) != 0) {
            err = "must start with " + pref;
            return false;
        }
        return true;
    }
};

struct CustomValidatedDto {
    [[=is_valid_role{}]]
    std::string role;

    [[=is_valid_role{"APP_"}]]
    std::string app_code;
};

TEST(ValidationTest, CustomValidatorPassesAndFails) {
    CustomValidatedDto valid_dto{"ROLE_ADMIN", "APP_PAY"};
    std::vector<std::string> errors;
    EXPECT_TRUE(novaboot::validation::validate(valid_dto, errors));
    EXPECT_EQ(errors.size(), 0);

    CustomValidatedDto invalid_dto{"ADMIN", "PAY"};
    std::vector<std::string> errs;
    EXPECT_FALSE(novaboot::validation::validate(invalid_dto, errs));
    EXPECT_EQ(errs.size(), 2);
    EXPECT_NE(errs[0].find("role must start with ROLE_"), std::string::npos);
    EXPECT_NE(errs[1].find("app_code must start with APP_"), std::string::npos);
}
