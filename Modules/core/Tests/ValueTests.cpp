#include <symbolengine/core/Value.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

using namespace symbolengine;

TEST_CASE("A value keeps the kind it was made from")
{
    CHECK(Value().isNull());
    CHECK(Value(nullptr).isNull());
    CHECK(Value(true).isBool());
    CHECK(Value(42).isInteger());
    CHECK(Value(std::int64_t{1} << 40).isInteger());
    CHECK(Value(0.5).isReal());
    CHECK(Value(0.5f).isReal());
    CHECK(Value("text").isString());
    CHECK(Value(std::string("text")).isString());
    CHECK(Value(std::string_view("text")).isString());
    CHECK(Value(Array{1, 2}).isArray());
    CHECK(Value(Object{{"a", 1}}).isObject());

    char writable[] = "text";
    CHECK(Value(writable).isString());
}

TEST_CASE("Sample positions beyond 32 bits stay exact")
{
    const std::int64_t twoHoursAt192k = 192000LL * 60 * 60 * 2 * 1000;
    CHECK(Value(twoHoursAt192k).asInt() == twoHoursAt192k);
    CHECK(Value(std::numeric_limits<std::int64_t>::max()).asInt() == std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("An unsigned number too large for a signed integer becomes a real")
{
    const Value huge(std::numeric_limits<std::uint64_t>::max());
    CHECK(huge.isReal());
    CHECK(huge.asNumber() == 18446744073709551616.0);
    CHECK(Value(std::uint64_t{7}).isInteger());
}

TEST_CASE("Reading with a fallback never throws")
{
    const Value text("hello");
    CHECK(text.intOr(-1) == -1);
    CHECK(text.numberOr(2.5) == 2.5);
    CHECK(text.boolOr(true));
    CHECK(text.stringOr("x") == "hello");
    CHECK(Value(3).stringOr("none") == "none");

    CHECK(Value(3.0).intOr(-1) == 3);
    CHECK(Value(3.5).intOr(-1) == -1);
    CHECK(Value(1e30).intOr(-1) == -1);
    CHECK(Value(std::nan("")).intOr(-1) == -1);
    CHECK(Value(7).numberOr(0) == 7.0);
}

TEST_CASE("Reading as the wrong kind throws TypeError saying what was found")
{
    const Value text("hello");
    CHECK_THROWS_AS(text.asInt(), TypeError);
    CHECK_THROWS_AS(text.asArray(), TypeError);
    CHECK_THROWS_AS(Value(2.5).asInt(), TypeError);
    CHECK_THROWS_AS(Value().asString(), TypeError);

    try
    {
        (void) text.asObject();
        FAIL("asObject should have thrown");
    }
    catch (const TypeError& error)
    {
        CHECK(std::string(error.what()) == "expected an object, found a string");
    }
}

TEST_CASE("A path into missing data reads as null")
{
    const Value value(Object{{"sources", Array{Object{{"path", "/take.wav"}}}}});
    CHECK(value["sources"][0]["path"].stringOr("") == "/take.wav");
    CHECK(value["sources"][1]["path"].isNull());
    CHECK(value["missing"]["deeper"][3].isNull());
    CHECK(Value(5)["anything"].isNull());
    CHECK(value["sources"].size() == 1);
    CHECK(value.size() == 1);
    CHECK(Value("text").size() == 0);
}

TEST_CASE("An object keeps its members in the order they were added")
{
    Object object;
    object["zebra"] = 1;
    object["apple"] = 2;
    object["mango"] = 3;
    object.set("apple", 20);

    std::string order;
    for (const auto& member : object)
        order += member.name() + " ";
    CHECK(order == "zebra apple mango ");
    CHECK(*object.find("apple") == Value(20));

    CHECK(object.erase("zebra"));
    CHECK_FALSE(object.erase("zebra"));
    CHECK(object.begin()->name() == "apple");
    CHECK(object.size() == 2);
}

TEST_CASE("A large object finds every member after adds and removals")
{
    Object object;
    for (int i = 0; i < 1000; ++i)
        object.set("key" + std::to_string(i), i);

    for (int i = 0; i < 1000; i += 3)
        CHECK(object.erase("key" + std::to_string(i)));

    for (int i = 0; i < 1000; ++i)
    {
        const auto* found = object.find("key" + std::to_string(i));
        if (i % 3 == 0)
            CHECK(found == nullptr);
        else
        {
            REQUIRE(found != nullptr);
            CHECK(found->asInt() == i);
        }
    }

    // Members after a removal keep their order.
    auto member = object.begin();
    CHECK(member->name() == "key1");
    ++member;
    CHECK(member->name() == "key2");
    ++member;
    CHECK(member->name() == "key4");

    const Object copy = object;
    CHECK(copy == object);
    CHECK(copy.find("key998") != nullptr);
    object.clear();
    CHECK(object.find("key998") == nullptr);
    CHECK(copy.find("key998") != nullptr);
}

TEST_CASE("A member's value can be changed through iteration")
{
    Object object{{"a", 1}, {"b", 2}};
    for (auto& member : object)
        member.value = member.value.asInt() * 10;
    CHECK(object == Object{{"a", 10}, {"b", 20}});
}

TEST_CASE("Values compare by contents, an integer equal to the same real")
{
    CHECK(Value(1) == Value(1.0));
    CHECK(Value(1.0) == Value(1));
    CHECK_FALSE(Value(1) == Value(1.5));
    CHECK_FALSE(Value(1) == Value("1"));
    CHECK_FALSE(Value() == Value(false));
    CHECK(Value(std::numeric_limits<std::int64_t>::max()) != Value(9223372036854775808.0));
    CHECK(Object{{"a", 1}, {"b", 2}} == Object{{"b", 2}, {"a", 1}});
    CHECK_FALSE(Object{{"a", 1}} == Object{{"a", 1}, {"b", 2}});
    CHECK(Value(Array{1, "two", Object{{"three", 3.0}}}) == Value(Array{1.0, "two", Object{{"three", 3}}}));
    CHECK_FALSE(Value(Array{1, 2}) == Value(Array{2, 1}));
}

TEST_CASE("Copies are independent")
{
    Value original(Object{{"list", Array{1, 2, 3}}});
    Value copy = original;
    copy.asObject()["list"].asArray().push_back(4);
    CHECK(original["list"].size() == 3);
    CHECK(copy["list"].size() == 4);
}
