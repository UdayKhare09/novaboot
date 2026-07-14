#include <gtest/gtest.h>
#include "novaboot/db/orm_reflection.h"

using namespace novaboot::db;
using namespace novaboot::annotations;

struct [[= Entity("custom_table") ]] TestEntity {
    [[= Id() ]]
    int id = 0;
    
    [[= Column("user_name") ]]
    std::string name;
    
    int age = 0;
};

TEST(OrmReflectionTest, MetadataExtraction) {
    constexpr auto ent = detail::get_table_metadata<TestEntity>();
    EXPECT_EQ(std::string_view(ent.name), "custom_table");
    EXPECT_EQ(std::string_view(detail::get_column_name<TestEntity, &TestEntity::id>().name), "id");
    EXPECT_EQ(std::string_view(detail::get_column_name<TestEntity, &TestEntity::name>().name), "user_name");
    EXPECT_EQ(std::string_view(detail::get_column_name<TestEntity, &TestEntity::age>().name), "age");
}

class MockResultSet : public ResultSet {
public:
    bool next() override { return false; }
    bool is_null(int) override { return false; }
    std::int64_t get_int(int col) override { return col == 0 ? 42 : 25; }
    double get_double(int) override { return 0.0; }
    std::string get_string(int col) override { return col == 1 ? "Alice" : ""; }
    std::vector<std::uint8_t> get_blob(int) override { return {}; }
    bool get_bool(int) override { return false; }
    Uuid get_uuid(int) override { return {}; }
    std::chrono::system_clock::time_point get_time(int) override { return {}; }
    int column_count() const override { return 3; }
    std::string_view column_name(int) const override { return ""; }
};

TEST(OrmReflectionTest, RowMapping) {
    MockResultSet rs;
    auto entity = detail::map_row_to_entity<TestEntity>(&rs);
    EXPECT_EQ(entity.id, 42);
    EXPECT_EQ(entity.name, "Alice");
    EXPECT_EQ(entity.age, 25);
}
