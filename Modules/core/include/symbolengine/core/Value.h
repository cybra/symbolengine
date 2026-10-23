#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace symbolengine
{

class Value;
class Member;

/** A list of values, as a JSON array holds. */
using Array = std::vector<Value>;

/** Named values kept in the order they were added, as a JSON object holds. Finding a name takes the same time however
    many members there are. */
class Object
{
public:
    Object();
    Object(std::initializer_list<Member> members);
    Object(const Object& other);
    Object(Object&& other) noexcept;
    Object& operator=(const Object& other);
    Object& operator=(Object&& other) noexcept;
    ~Object();

    std::size_t size() const noexcept;
    bool empty() const noexcept;
    bool contains(std::string_view name) const;

    /** The value with this name, or nullptr if there is none. */
    const Value* find(std::string_view name) const;
    Value* find(std::string_view name);

    /** The value with this name, added at the end as null if there is none. */
    Value& operator[](std::string_view name);

    /** Sets the value with this name, keeping its place if it is there already and adding it at the end if not. */
    Value& set(std::string_view name, Value value);

    /** Removes the value with this name, and says whether there was one. The members after it keep their order. */
    bool erase(std::string_view name);

    void clear() noexcept;

    /** The members in order. A member's value can be changed through these; its name can't. */
    Member* begin() noexcept;
    Member* end() noexcept;
    const Member* begin() const noexcept;
    const Member* end() const noexcept;

    /** Two objects are equal when they have the same names with equal values, in any order. */
    bool operator==(const Object& other) const;

private:
    struct Index;

    std::vector<Member> members;
    std::unique_ptr<Index> index;   // made once there are enough members for it to be quicker

    void reindex();
};

/** Thrown when a value is read as a kind it isn't, such as a string read as an integer. */
class TypeError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/** A value of the kinds JSON holds: null, true or false, a number, text, an array or an object. Numbers are kept as a
    64-bit integer when they are whole and fit, and as a double otherwise, so sample positions and lengths stay exact.
    Text is UTF-8.

    Reading never changes a value: a missing member or item reads as null, so a path into data that may not be there,
    such as value["sources"][0]["path"].stringOr(""), is safe. Values are built with Object and Array and changed
    through asObject() and asArray(). */
class Value
{
public:
    enum class Type
    {
        null,
        boolean,
        integer,
        real,
        string,
        array,
        object
    };

    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool b) noexcept : data(b) {}

    /** A whole number. An unsigned number too large for a 64-bit signed integer is kept as a double. */
    template <std::integral T>
        requires(!std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, wchar_t> &&
                 !std::same_as<T, char8_t> && !std::same_as<T, char16_t> && !std::same_as<T, char32_t>)
    Value(T number) noexcept
    {
        if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t))
        {
            if (number > static_cast<T>(std::numeric_limits<std::int64_t>::max()))
            {
                data = static_cast<double>(number);
                return;
            }
        }
        data = static_cast<std::int64_t>(number);
    }

    template <std::floating_point T>
    Value(T number) noexcept : data(static_cast<double>(number))
    {
    }

    Value(std::string text) noexcept : data(std::move(text)) {}
    Value(std::string_view text) : data(std::string(text)) {}
    Value(const char* text) : data(std::string(text)) {}
    Value(Array array) noexcept : data(std::move(array)) {}
    Value(Object object) noexcept : data(std::move(object)) {}

    /** A pointer of any other kind would otherwise become true or false. */
    template <typename T>
        requires(!std::same_as<std::remove_cv_t<T>, char>)
    Value(T*) = delete;

    Type type() const noexcept { return static_cast<Type>(data.index()); }

    bool isNull() const noexcept { return type() == Type::null; }
    bool isBool() const noexcept { return type() == Type::boolean; }
    bool isInteger() const noexcept { return type() == Type::integer; }
    bool isReal() const noexcept { return type() == Type::real; }
    bool isNumber() const noexcept { return isInteger() || isReal(); }
    bool isString() const noexcept { return type() == Type::string; }
    bool isArray() const noexcept { return type() == Type::array; }
    bool isObject() const noexcept { return type() == Type::object; }

    /** Reading with a fallback, for anything missing or of another kind. A real reads as an integer only when it is
        whole and fits. */
    bool boolOr(bool fallback) const noexcept;
    std::int64_t intOr(std::int64_t fallback) const noexcept;
    double numberOr(double fallback) const noexcept;
    std::string stringOr(std::string_view fallback) const;

    /** The value itself if it is of this kind, or nullptr. */
    const std::string* ifString() const noexcept { return std::get_if<std::string>(&data); }
    const Array* ifArray() const noexcept { return std::get_if<Array>(&data); }
    const Object* ifObject() const noexcept { return std::get_if<Object>(&data); }
    Array* ifArray() noexcept { return std::get_if<Array>(&data); }
    Object* ifObject() noexcept { return std::get_if<Object>(&data); }

    /** Reading as a kind the value must be; each throws TypeError if it isn't. */
    bool asBool() const;
    std::int64_t asInt() const;
    double asNumber() const;
    const std::string& asString() const;
    const Array& asArray() const;
    const Object& asObject() const;
    std::string& asString();
    Array& asArray();
    Object& asObject();

    /** A member of an object, or an item of an array; null if there is no such member or item. */
    const Value& operator[](std::string_view name) const noexcept;
    const Value& operator[](std::size_t index) const noexcept;

    /** The number of members of an object or items of an array, and 0 for anything else. */
    std::size_t size() const noexcept;

    /** Values are equal when they are of the same kind with equal contents, except that an integer and a real are
        equal when they are the same number. */
    bool operator==(const Value& other) const;

    /** The kind's name, for messages: "null", "a boolean", "an integer", "a real", "a string", "an array" or
        "an object". */
    static const char* describe(Type type) noexcept;

private:
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> data;
};

/** A name and its value in an Object. */
class Member
{
public:
    Member(std::string name, Value memberValue) : value(std::move(memberValue)), key(std::move(name)) {}

    const std::string& name() const noexcept { return key; }

    Value value;

private:
    friend class Object;
    std::string key;
};

}
