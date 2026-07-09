#include <gtest/gtest.h>
#include "novaboot/novaboot.h"

struct Address {
    std::string street;
    int zip;
};

struct Person {
    std::string name;
    int age;
    bool active;
    Address address;
    std::vector<std::string> tags;
};

TEST(JSONReflectionTest, SerializeDeserialize) {
    Person p;
    p.name = "Alice";
    p.age = 30;
    p.active = true;
    p.address = {"Main St", 12345};
    p.tags = {"developer", "cpp"};

    // 1. Serialize
    std::string json = novaboot::json::serialize(p);
    
    // Check key strings are present in output
    EXPECT_NE(json.find("\"name\":\"Alice\""), std::string::npos);
    EXPECT_NE(json.find("\"age\":30"), std::string::npos);
    EXPECT_NE(json.find("\"active\":true"), std::string::npos);
    EXPECT_NE(json.find("\"street\":\"Main St\""), std::string::npos);
    EXPECT_NE(json.find("\"zip\":12345"), std::string::npos);
    EXPECT_NE(json.find("[\"developer\",\"cpp\"]"), std::string::npos);

    // 2. Deserialize
    Person p2 = novaboot::json::deserialize<Person>(json);
    EXPECT_EQ(p2.name, "Alice");
    EXPECT_EQ(p2.age, 30);
    EXPECT_EQ(p2.active, true);
    EXPECT_EQ(p2.address.street, "Main St");
    EXPECT_EQ(p2.address.zip, 12345);
    ASSERT_EQ(p2.tags.size(), 2u);
    EXPECT_EQ(p2.tags[0], "developer");
    EXPECT_EQ(p2.tags[1], "cpp");
}
