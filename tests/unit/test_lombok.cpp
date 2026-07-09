#include <gtest/gtest.h>
#include "model/user.h"
#include <string>

using namespace examples::model;

TEST(LombokTest, NoArgsConstructorInstantiates) {
    User u;
    u.set_id(10);
    EXPECT_EQ(u.get_id(), 10);
}

TEST(LombokTest, AllArgsConstructorInitializes) {
    User u(1, "Alice", "alice@example.com", "ROLE_USER");
    EXPECT_EQ(u.get_id(), 1);
    EXPECT_EQ(u.get_name(), "Alice");
    EXPECT_EQ(u.get_email(), "alice@example.com");
    EXPECT_EQ(u.get_role(), "ROLE_USER");
}

TEST(LombokTest, GettersAndSettersWork) {
    User u;
    u.set_id(42);
    u.set_name("Bob");
    u.set_email("bob@example.com");
    u.set_role("ROLE_ADMIN");

    EXPECT_EQ(u.get_id(), 42);
    EXPECT_EQ(u.get_name(), "Bob");
    EXPECT_EQ(u.get_email(), "bob@example.com");
    EXPECT_EQ(u.get_role(), "ROLE_ADMIN");
}

TEST(LombokTest, BuilderPatternsBuildsCorrectly) {
    auto u = User::builder()
        .id(7)
        .name("Charlie")
        .email("charlie@example.com")
        .role("ROLE_USER")
        .build();

    EXPECT_EQ(u.get_id(), 7);
    EXPECT_EQ(u.get_name(), "Charlie");
    EXPECT_EQ(u.get_email(), "charlie@example.com");
    EXPECT_EQ(u.get_role(), "ROLE_USER");
}

TEST(LombokTest, ToStringPrintsAttributes) {
    auto u = User::builder().id(3).name("Dan").email("dan@example.com").role("ROLE_USER").build();
    std::string s = u.to_string();
    EXPECT_NE(s.find("User("), std::string::npos);
    EXPECT_NE(s.find("id=3"), std::string::npos);
    EXPECT_NE(s.find("name=Dan"), std::string::npos);
}

TEST(LombokTest, EqualsChecksEquality) {
    auto u1 = User::builder().id(1).name("A").email("a@b.com").role("ROLE_USER").build();
    auto u2 = User::builder().id(1).name("A").email("a@b.com").role("ROLE_USER").build();
    auto u3 = User::builder().id(2).name("A").email("a@b.com").role("ROLE_USER").build();

    EXPECT_TRUE(u1 == u2);
    EXPECT_FALSE(u1 == u3);
    EXPECT_TRUE(u1 != u3);
}

TEST(LombokTest, HashCodeGenerates) {
    auto u1 = User::builder().id(1).name("A").email("a@b.com").role("ROLE_USER").build();
    auto u2 = User::builder().id(1).name("A").email("a@b.com").role("ROLE_USER").build();
    EXPECT_EQ(u1.hash_code(), u2.hash_code());
}
