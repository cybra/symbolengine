#include "symbolengine/core/Value.h"

#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace symbolengine
{

namespace
{
// Below this many members a straight search is as quick as a hash table, and costs nothing to keep.
constexpr std::size_t indexFrom = 16;

struct NameHash
{
    using is_transparent = void;
    std::size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
};

const Value& nullValue() noexcept
{
    static const Value none;
    return none;
}

// A real as an integer, when it is whole and fits.
bool wholeNumber(double real, std::int64_t& integer) noexcept
{
    constexpr double limit = 9223372036854775808.0;   // 2^63
    if (!(real >= -limit && real < limit) || std::trunc(real) != real)
        return false;
    integer = static_cast<std::int64_t>(real);
    return true;
}

[[noreturn]] void wrongType(Value::Type wanted, Value::Type found)
{
    throw TypeError(std::string("expected ") + Value::describe(wanted) + ", found " + Value::describe(found));
}
}

struct Object::Index
{
    std::unordered_map<std::string, std::size_t, NameHash, std::equal_to<>> positions;
};

Object::Object() = default;

Object::Object(std::initializer_list<Member> list)
{
    for (const auto& member : list)
        set(member.name(), member.value);
}

Object::Object(const Object& other) : members(other.members)
{
    reindex();
}

Object::Object(Object&& other) noexcept = default;

Object& Object::operator=(const Object& other)
{
    if (this != &other)
    {
        members = other.members;
        reindex();
    }
    return *this;
}

Object& Object::operator=(Object&& other) noexcept = default;

Object::~Object() = default;

std::size_t Object::size() const noexcept
{
    return members.size();
}

bool Object::empty() const noexcept
{
    return members.empty();
}

bool Object::contains(std::string_view name) const
{
    return find(name) != nullptr;
}

const Value* Object::find(std::string_view name) const
{
    if (index != nullptr)
    {
        auto found = index->positions.find(name);
        return found != index->positions.end() ? &members[found->second].value : nullptr;
    }

    for (const auto& member : members)
        if (member.key == name)
            return &member.value;

    return nullptr;
}

Value* Object::find(std::string_view name)
{
    return const_cast<Value*>(std::as_const(*this).find(name));
}

Value& Object::operator[](std::string_view name)
{
    if (auto* existing = find(name))
        return *existing;
    return set(name, Value());
}

Value& Object::set(std::string_view name, Value value)
{
    if (auto* existing = find(name))
    {
        *existing = std::move(value);
        return *existing;
    }

    members.emplace_back(std::string(name), std::move(value));

    if (index != nullptr)
        index->positions.emplace(members.back().key, members.size() - 1);
    else if (members.size() >= indexFrom)
        reindex();

    return members.back().value;
}

bool Object::erase(std::string_view name)
{
    for (auto member = members.begin(); member != members.end(); ++member)
    {
        if (member->key == name)
        {
            members.erase(member);
            reindex();
            return true;
        }
    }
    return false;
}

void Object::clear() noexcept
{
    members.clear();
    index.reset();
}

Member* Object::begin() noexcept
{
    return members.data();
}

Member* Object::end() noexcept
{
    return members.data() + members.size();
}

const Member* Object::begin() const noexcept
{
    return members.data();
}

const Member* Object::end() const noexcept
{
    return members.data() + members.size();
}

bool Object::operator==(const Object& other) const
{
    if (members.size() != other.members.size())
        return false;

    for (const auto& member : members)
    {
        const auto* theirs = other.find(member.key);
        if (theirs == nullptr || !(*theirs == member.value))
            return false;
    }
    return true;
}

void Object::reindex()
{
    if (members.size() < indexFrom)
    {
        index.reset();
        return;
    }

    if (index == nullptr)
        index = std::make_unique<Index>();

    index->positions.clear();
    index->positions.reserve(members.size());

    for (std::size_t i = 0; i < members.size(); ++i)
        index->positions.emplace(members[i].key, i);
}

bool Value::boolOr(bool fallback) const noexcept
{
    const auto* b = std::get_if<bool>(&data);
    return b != nullptr ? *b : fallback;
}

std::int64_t Value::intOr(std::int64_t fallback) const noexcept
{
    if (const auto* integer = std::get_if<std::int64_t>(&data))
        return *integer;

    std::int64_t whole = 0;
    if (const auto* real = std::get_if<double>(&data); real != nullptr && wholeNumber(*real, whole))
        return whole;

    return fallback;
}

double Value::numberOr(double fallback) const noexcept
{
    if (const auto* integer = std::get_if<std::int64_t>(&data))
        return static_cast<double>(*integer);
    if (const auto* real = std::get_if<double>(&data))
        return *real;
    return fallback;
}

std::string Value::stringOr(std::string_view fallback) const
{
    const auto* text = std::get_if<std::string>(&data);
    return text != nullptr ? *text : std::string(fallback);
}

bool Value::asBool() const
{
    if (const auto* b = std::get_if<bool>(&data))
        return *b;
    wrongType(Type::boolean, type());
}

std::int64_t Value::asInt() const
{
    if (const auto* integer = std::get_if<std::int64_t>(&data))
        return *integer;

    if (const auto* real = std::get_if<double>(&data))
    {
        std::int64_t whole = 0;
        if (wholeNumber(*real, whole))
            return whole;
        throw TypeError("expected an integer, found a real that isn't a whole number in range");
    }

    wrongType(Type::integer, type());
}

double Value::asNumber() const
{
    if (const auto* integer = std::get_if<std::int64_t>(&data))
        return static_cast<double>(*integer);
    if (const auto* real = std::get_if<double>(&data))
        return *real;
    wrongType(Type::real, type());
}

const std::string& Value::asString() const
{
    if (const auto* text = std::get_if<std::string>(&data))
        return *text;
    wrongType(Type::string, type());
}

const Array& Value::asArray() const
{
    if (const auto* array = std::get_if<Array>(&data))
        return *array;
    wrongType(Type::array, type());
}

const Object& Value::asObject() const
{
    if (const auto* object = std::get_if<Object>(&data))
        return *object;
    wrongType(Type::object, type());
}

std::string& Value::asString()
{
    return const_cast<std::string&>(std::as_const(*this).asString());
}

Array& Value::asArray()
{
    return const_cast<Array&>(std::as_const(*this).asArray());
}

Object& Value::asObject()
{
    return const_cast<Object&>(std::as_const(*this).asObject());
}

const Value& Value::operator[](std::string_view name) const noexcept
{
    if (const auto* object = std::get_if<Object>(&data))
        if (const auto* member = object->find(name))
            return *member;
    return nullValue();
}

const Value& Value::operator[](std::size_t i) const noexcept
{
    if (const auto* array = std::get_if<Array>(&data); array != nullptr && i < array->size())
        return (*array)[i];
    return nullValue();
}

std::size_t Value::size() const noexcept
{
    if (const auto* array = std::get_if<Array>(&data))
        return array->size();
    if (const auto* object = std::get_if<Object>(&data))
        return object->size();
    return 0;
}

bool Value::operator==(const Value& other) const
{
    const auto* integer = std::get_if<std::int64_t>(&data);
    const auto* real = std::get_if<double>(&other.data);

    if (integer == nullptr || real == nullptr)
    {
        integer = std::get_if<std::int64_t>(&other.data);
        real = std::get_if<double>(&data);
    }

    if (integer != nullptr && real != nullptr)
    {
        std::int64_t whole = 0;
        return wholeNumber(*real, whole) && whole == *integer;
    }

    return data == other.data;
}

const char* Value::describe(Type type) noexcept
{
    switch (type)
    {
        case Type::null: return "null";
        case Type::boolean: return "a boolean";
        case Type::integer: return "an integer";
        case Type::real: return "a real";
        case Type::string: return "a string";
        case Type::array: return "an array";
        case Type::object: return "an object";
    }
    return "an unknown kind";
}

}
