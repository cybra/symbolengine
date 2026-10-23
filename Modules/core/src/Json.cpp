#include "symbolengine/core/Json.h"

#include "symbolengine/core/Text.h"

#include "Numbers.h"
#include "Utf8.h"

#include <charconv>
#include <cmath>

namespace symbolengine::json
{

namespace
{
void writeEscaped(std::string& out, std::string_view text)
{
    constexpr char hex[] = "0123456789abcdef";
    std::size_t runStart = 0;

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;

        out.append(text.substr(runStart, i - runStart));
        runStart = i + 1;

        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0xF];
                break;
        }
    }

    out.append(text.substr(runStart));
}

void writeString(std::string& out, std::string_view text)
{
    out += '"';
    if (utf8::firstInvalid(text) == text.size())
        writeEscaped(out, text);
    else
        writeEscaped(out, text::replaceInvalidUtf8(text));
    out += '"';
}

void writeReal(std::string& out, double real)
{
    if (!std::isfinite(real))
    {
        out += "null";
        return;
    }

    // From 2^53 up, doubles can't hold every whole number, so a whole real written as digits alone could read
    // back as a nearby integer instead. Written with an exponent, it reads back as the same double.
    constexpr double wholeLimit = 9007199254740992.0;
    if (std::fabs(real) < wholeLimit || std::trunc(real) != real)
    {
        out += text::formatNumber(real);
        return;
    }

    const auto decimal = numbers::shortest(std::fabs(real));
    if (real < 0)
        out += '-';
    out += decimal.digits[0];
    if (decimal.count > 1)
    {
        out += '.';
        out.append(decimal.digits + 1, static_cast<std::size_t>(decimal.count - 1));
    }
    out += "e+";
    out += std::to_string(decimal.exponent - 1);
}

void newLine(std::string& out, std::size_t depth)
{
    out += '\n';
    out.append(depth * 2, ' ');
}

void writeValue(std::string& out, const Value& value, Layout layout, std::size_t depth)
{
    const bool pretty = layout == Layout::pretty;

    switch (value.type())
    {
        case Value::Type::null: out += "null"; break;
        case Value::Type::boolean: out += value.asBool() ? "true" : "false"; break;

        case Value::Type::integer:
        {
            char buffer[24];
            const auto result = std::to_chars(buffer, buffer + sizeof buffer, value.asInt());
            out.append(buffer, result.ptr);
            break;
        }

        case Value::Type::real: writeReal(out, value.asNumber()); break;

        case Value::Type::string: writeString(out, value.asString()); break;

        case Value::Type::array:
        {
            const auto& array = value.asArray();
            out += '[';
            for (std::size_t i = 0; i < array.size(); ++i)
            {
                if (i > 0)
                    out += ',';
                if (pretty)
                    newLine(out, depth + 1);
                writeValue(out, array[i], layout, depth + 1);
            }
            if (pretty && !array.empty())
                newLine(out, depth);
            out += ']';
            break;
        }

        case Value::Type::object:
        {
            const auto& object = value.asObject();
            out += '{';
            bool first = true;
            for (const auto& member : object)
            {
                if (!first)
                    out += ',';
                first = false;
                if (pretty)
                    newLine(out, depth + 1);
                writeString(out, member.name());
                out += pretty ? ": " : ":";
                writeValue(out, member.value, layout, depth + 1);
            }
            if (pretty && !object.empty())
                newLine(out, depth);
            out += '}';
            break;
        }
    }
}

class Parser
{
public:
    explicit Parser(std::string_view json) : text(json) {}

    Value document()
    {
        if (text.starts_with("\xEF\xBB\xBF"))
            position = 3;

        skipSpace();
        Value value = parseValue(0);
        skipSpace();

        if (position < text.size())
            fail("there is more after the value");

        return value;
    }

private:
    std::string_view text;
    std::size_t position = 0;

    [[noreturn]] void fail(const std::string& reason) const
    {
        std::size_t line = 1, lineStart = 0;
        for (std::size_t i = 0; i < position && i < text.size(); ++i)
        {
            if (text[i] == '\n')
            {
                ++line;
                lineStart = i + 1;
            }
        }
        throw ParseError(reason, line, position - lineStart + 1);
    }

    bool atEnd() const noexcept { return position >= text.size(); }
    char next() const noexcept { return atEnd() ? '\0' : text[position]; }

    void skipSpace() noexcept
    {
        while (!atEnd() &&
               (text[position] == ' ' || text[position] == '\t' || text[position] == '\n' || text[position] == '\r'))
            ++position;
    }

    Value parseValue(std::size_t depth)
    {
        if (atEnd())
            fail("the text ends where a value should be");

        switch (text[position])
        {
            case '{': return parseObject(depth + 1);
            case '[': return parseArray(depth + 1);
            case '"': return parseString();
            case 't': expectWord("true"); return true;
            case 'f': expectWord("false"); return false;
            case 'n': expectWord("null"); return nullptr;
            default: break;
        }

        if (next() == '-' || (next() >= '0' && next() <= '9'))
            return parseNumber();

        fail("a value can't start with this character");
    }

    void expectWord(std::string_view word)
    {
        if (text.substr(position, word.size()) != word)
            fail("a value can't start with this character");
        position += word.size();
    }

    void checkDepth(std::size_t depth) const
    {
        if (depth > maximumDepth)
            fail("arrays and objects are nested more than " + std::to_string(maximumDepth) + " deep");
    }

    Value parseObject(std::size_t depth)
    {
        checkDepth(depth);
        ++position;
        skipSpace();

        Object object;
        if (next() == '}')
        {
            ++position;
            return object;
        }

        for (;;)
        {
            if (next() != '"')
                fail(atEnd() ? "the text ends inside an object" : "expected a name in double quotes");

            std::string name = parseString();
            skipSpace();

            if (next() != ':')
                fail(atEnd() ? "the text ends inside an object" : "expected a colon after the name");

            ++position;
            skipSpace();
            object.set(name, parseValue(depth));
            skipSpace();

            if (next() == ',')
            {
                ++position;
                skipSpace();
                continue;
            }
            if (next() == '}')
            {
                ++position;
                return object;
            }
            fail(atEnd() ? "the text ends inside an object" : "expected a comma or a closing brace");
        }
    }

    Value parseArray(std::size_t depth)
    {
        checkDepth(depth);
        ++position;
        skipSpace();

        Array array;
        if (next() == ']')
        {
            ++position;
            return array;
        }

        for (;;)
        {
            array.push_back(parseValue(depth));
            skipSpace();

            if (next() == ',')
            {
                ++position;
                skipSpace();
                continue;
            }
            if (next() == ']')
            {
                ++position;
                return array;
            }
            fail(atEnd() ? "the text ends inside an array" : "expected a comma or a closing bracket");
        }
    }

    unsigned readHex4()
    {
        unsigned unit = 0;
        for (int i = 0; i < 4; ++i, ++position)
        {
            const char c = next();
            unit <<= 4;
            if (c >= '0' && c <= '9')
                unit |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                unit |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                unit |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail("\\u must be followed by four hexadecimal digits");
        }
        return unit;
    }

    void parseEscape(std::string& out)
    {
        ++position;   // the backslash
        const char c = next();
        ++position;

        switch (c)
        {
            case '"': out += '"'; return;
            case '\\': out += '\\'; return;
            case '/': out += '/'; return;
            case 'b': out += '\b'; return;
            case 'f': out += '\f'; return;
            case 'n': out += '\n'; return;
            case 'r': out += '\r'; return;
            case 't': out += '\t'; return;
            case 'u': break;
            default:
                --position;
                fail(atEnd() ? "the text ends inside a string" : "a backslash must be followed by an escape");
        }

        const unsigned unit = readHex4();

        if (unit >= 0xD800 && unit <= 0xDBFF && text.substr(position, 2) == "\\u")
        {
            const std::size_t pairStart = position;
            position += 2;
            const unsigned low = readHex4();
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                utf8::append(out, static_cast<char32_t>(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00)));
                return;
            }
            position = pairStart;   // the next escape stands on its own
        }

        utf8::append(out, static_cast<char32_t>(unit));   // a lone surrogate becomes U+FFFD
    }

    std::string parseString()
    {
        ++position;   // the opening quote
        std::string out;
        std::size_t runStart = position;

        for (;;)
        {
            if (atEnd())
                fail("the text ends inside a string");

            const auto c = static_cast<unsigned char>(text[position]);

            if (c == '"')
            {
                out.append(text.substr(runStart, position - runStart));
                ++position;
                return out;
            }

            if (c == '\\')
            {
                out.append(text.substr(runStart, position - runStart));
                parseEscape(out);
                runStart = position;
            }
            else if (c < 0x20)
                fail("a control character in a string must be escaped");
            else if (c >= 0x80)
            {
                const auto length = utf8::sequenceLength(text, position);
                if (length == 0)
                    fail("the text isn't valid UTF-8");
                position += length;
            }
            else
                ++position;
        }
    }

    Value parseNumber()
    {
        bool integral = false;
        const auto length = numbers::jsonNumberLength(text.substr(position), &integral);
        if (length == 0)
            fail("a number is written wrongly");

        const auto number = text.substr(position, length);
        position += length;

        if (integral)
        {
            std::int64_t integer = 0;
            const auto result = std::from_chars(number.data(), number.data() + number.size(), integer);
            if (result.ec == std::errc() && result.ptr == number.data() + number.size())
            {
                if (integer == 0 && number.front() == '-')
                    return -0.0;
                return integer;
            }
        }

        return numbers::read(number);
    }
};
}

std::string write(const Value& value, Layout layout)
{
    std::string out;
    writeValue(out, value, layout, 0);
    return out;
}

ParseError::ParseError(const std::string& reason, std::size_t line, std::size_t column)
    : std::runtime_error("JSON line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + reason),
      why(reason), where{line, column}
{
}

Value parse(std::string_view text)
{
    return Parser(text).document();
}

std::optional<Value> tryParse(std::string_view text)
{
    try
    {
        return parse(text);
    }
    catch (const ParseError&)
    {
        return std::nullopt;
    }
}

}
