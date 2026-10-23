#include "symbolengine/core/Xml.h"

#include "symbolengine/core/Text.h"

#include "Utf8.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace symbolengine::xml
{

namespace
{
char32_t decode(std::string_view text, std::size_t at, std::size_t length) noexcept
{
    const auto byte = [&](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(text[at + i])); };

    switch (length)
    {
        case 1: return byte(0);
        case 2: return ((byte(0) & 0x1F) << 6) | (byte(1) & 0x3F);
        case 3: return ((byte(0) & 0x0F) << 12) | ((byte(1) & 0x3F) << 6) | (byte(2) & 0x3F);
        default:
            return ((byte(0) & 0x07) << 18) | ((byte(1) & 0x3F) << 12) | ((byte(2) & 0x3F) << 6) | (byte(3) & 0x3F);
    }
}

// XML 1.0's NameStartChar and NameChar.
bool isNameStart(char32_t c) noexcept
{
    return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) ||
           (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D) ||
           (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) ||
           (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
           (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

bool isNameChar(char32_t c) noexcept
{
    return isNameStart(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 ||
           (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

// The length in bytes of the name starting here, or 0 if no name starts here.
std::size_t nameLength(std::string_view text, std::size_t start) noexcept
{
    std::size_t at = start;

    while (at < text.size())
    {
        const auto length = utf8::sequenceLength(text, at);
        if (length == 0)
            break;

        const auto c = decode(text, at, length);
        if (!(at == start ? isNameStart(c) : isNameChar(c)))
            break;

        at += length;
    }

    return at - start;
}

std::string checkedName(std::string_view name)
{
    if (name.empty() || nameLength(name, 0) != name.size())
        throw std::invalid_argument("\"" + std::string(name) + "\" isn't a name XML allows");
    return std::string(name);
}

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void writeReference(std::string& out, char32_t c)
{
    out += "&#";
    out += std::to_string(static_cast<unsigned long>(c));
    out += ';';
}

void writeEscapedValid(std::string& out, std::string_view text, bool attribute)
{
    std::size_t runStart = 0;

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool notAChar = c == 0xEF && i + 2 < text.size() && text[i + 1] == '\xBF' &&
                              (text[i + 2] == '\xBE' || text[i + 2] == '\xBF');   // U+FFFE and U+FFFF

        if (c >= 0x20 && c != '&' && c != '<' && c != '>' && !(attribute && c == '"') && !notAChar)
            continue;
        if (c == '\t' && !attribute)
            continue;
        if (c == '\n' && !attribute)
            continue;

        out.append(text.substr(runStart, i - runStart));
        runStart = i + 1;

        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                if (notAChar)
                {
                    writeReference(out, decode(text, i, 3));
                    i += 2;
                    runStart = i + 1;
                }
                else
                    writeReference(out, c);
                break;
        }
    }

    out.append(text.substr(runStart));
}

void writeEscaped(std::string& out, std::string_view text, bool attribute)
{
    if (utf8::firstInvalid(text) == text.size())
        writeEscapedValid(out, text, attribute);
    else
        writeEscapedValid(out, text::replaceInvalidUtf8(text), attribute);
}

void writeElement(std::string& out, const Element& element, Layout layout, std::size_t depth)
{
    const bool pretty = layout == Layout::pretty;

    out += '<';
    out += element.name();
    for (const auto& attribute : element.attributes())
    {
        out += ' ';
        out += attribute.name;
        out += "=\"";
        writeEscaped(out, attribute.value, true);
        out += '"';
    }

    if (element.children().empty() && element.text().empty())
    {
        out += "/>";
        return;
    }

    out += '>';
    writeEscaped(out, element.text(), false);

    // Laid out, an element's text would gain the layout's spaces and line breaks, so one with text is written
    // compact inside.
    const bool indent = pretty && element.text().empty();
    for (const auto& child : element.children())
    {
        if (indent)
        {
            out += '\n';
            out.append((depth + 1) * 2, ' ');
        }
        writeElement(out, child, indent ? layout : Layout::compact, depth + 1);
    }
    if (indent && !element.children().empty())
    {
        out += '\n';
        out.append(depth * 2, ' ');
    }

    out += "</";
    out += element.name();
    out += '>';
}

class Parser
{
public:
    explicit Parser(std::string_view xml) : text(xml) {}

    Element document()
    {
        if (text.starts_with("\xEF\xBB\xBF"))
            position = 3;

        if (const auto invalid = utf8::firstInvalid(text); invalid < text.size())
        {
            position = invalid;
            fail("the text isn't valid UTF-8");
        }

        if (text.substr(position, 5) == "<?xml" && position + 5 < text.size() && isSpace(text[position + 5]))
            declaration();

        misc(true);
        if (next() != '<')
            fail(atEnd() ? "the text ends before the document's element" : "expected the document's element");

        Element root = rootElement();
        misc(false);

        if (!atEnd())
            fail("there is more after the document's element");

        return root;
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
    bool startsWith(std::string_view what) const noexcept { return text.substr(position, what.size()) == what; }

    bool skipSpace() noexcept
    {
        const auto start = position;
        while (!atEnd() && isSpace(text[position]))
            ++position;
        return position != start;
    }

    // Moves past the end marker, failing with the reason if the text ends first.
    std::string_view through(std::string_view end, const char* reason)
    {
        const auto found = text.find(end, position);
        if (found == std::string_view::npos)
        {
            position = text.size();
            fail(reason);
        }
        const auto inside = text.substr(position, found - position);
        position = found + end.size();
        return inside;
    }

    void declaration()
    {
        const auto start = position;
        const auto inside = through("?>", "the text ends inside the XML declaration");
        const auto at = inside.find("encoding");

        if (at == std::string_view::npos)
            return;

        auto rest = inside.substr(at + 8);
        while (!rest.empty() && (isSpace(rest.front()) || rest.front() == '='))
            rest.remove_prefix(1);
        if (rest.empty() || (rest.front() != '"' && rest.front() != '\''))
            return;

        const auto quote = rest.front();
        rest.remove_prefix(1);
        const auto encoding = rest.substr(0, rest.find(quote));

        if (!text::equalsIgnoringCaseAscii(encoding, "UTF-8") && !text::equalsIgnoringCaseAscii(encoding, "US-ASCII"))
        {
            position = start;
            fail("the document says it is in " + std::string(encoding) + ", and only UTF-8 is read");
        }
    }

    // Spaces, comments and processing instructions, and before the document's element a document type declaration.
    void misc(bool beforeRoot)
    {
        for (;;)
        {
            skipSpace();
            if (startsWith("<!--"))
                comment();
            else if (startsWith("<?"))
                instruction();
            else if (beforeRoot && startsWith("<!DOCTYPE"))
            {
                documentType();
                beforeRoot = false;
            }
            else
                return;
        }
    }

    void comment()
    {
        position += 4;
        through("-->", "the text ends inside a comment");
    }

    void instruction()
    {
        position += 2;
        const auto length = nameLength(text, position);
        if (length == 0)
            fail("expected a processing instruction's name");
        if (text::equalsIgnoringCaseAscii(text.substr(position, length), "xml"))
            fail("the XML declaration must come first");
        through("?>", "the text ends inside a processing instruction");
    }

    // Read past, with its own declarations: they are never used.
    void documentType()
    {
        int brackets = 0;
        position += 9;

        while (!atEnd())
        {
            const char c = text[position];

            if (c == '"' || c == '\'')
            {
                ++position;
                through(std::string_view(&c, 1), "the text ends inside the document type declaration");
                continue;
            }
            if (startsWith("<!--"))
            {
                comment();
                continue;
            }

            ++position;
            if (c == '[')
                ++brackets;
            else if (c == ']')
                --brackets;
            else if (c == '>' && brackets <= 0)
                return;
        }

        fail("the text ends inside the document type declaration");
    }

    std::string_view name(const char* reason)
    {
        const auto length = nameLength(text, position);
        if (length == 0)
            fail(atEnd() ? "the text ends inside a tag" : reason);
        const auto found = text.substr(position, length);
        position += length;
        return found;
    }

    // A start tag with its attributes, and whether it was also the end of the element, as <name/> is.
    std::pair<Element, bool> startTag()
    {
        ++position;   // the <
        Element element(name("expected an element's name"));

        for (;;)
        {
            const bool spaced = skipSpace();

            if (startsWith("/>"))
            {
                position += 2;
                return {std::move(element), true};
            }
            if (next() == '>')
            {
                ++position;
                return {std::move(element), false};
            }
            if (atEnd())
                fail("the text ends inside a tag");
            if (!spaced)
                fail("expected a space, or the end of the tag");

            const auto start = position;
            const auto attribute = name("expected an attribute, or the end of the tag");
            skipSpace();
            if (next() != '=')
                fail(atEnd() ? "the text ends inside a tag" : "expected an equals sign after the attribute's name");
            ++position;
            skipSpace();

            const char quote = next();
            if (quote != '"' && quote != '\'')
                fail(atEnd() ? "the text ends inside a tag" : "an attribute's value must be in quotes");
            ++position;
            auto value = attributeValue(quote);

            if (element.attribute(attribute) != nullptr)
            {
                position = start;
                fail("the attribute " + std::string(attribute) + " is given twice");
            }
            element.setAttribute(attribute, value);
        }
    }

    // The document's element and everything inside it. The elements still open are kept in a list rather than on
    // the stack, so how deep they go never depends on the size of the thread's stack, which is 1 MB on Windows.
    Element rootElement()
    {
        struct Open
        {
            Element element;
            std::string content;
        };
        std::vector<Open> open;

        if (auto [root, closed] = startTag(); closed)
            return std::move(root);
        else
            open.push_back({std::move(root), {}});

        for (;;)
        {
            auto& [element, content] = open.back();

            if (atEnd())
                fail("the text ends inside <" + element.name() + ">");

            if (next() == '&')
                reference(content);
            else if (next() != '<')
                characters(content, false);
            else if (startsWith("</"))
            {
                const auto start = position;
                position += 2;
                const auto closing = name("expected an element's name");
                if (closing != element.name())
                {
                    position = start;
                    fail("<" + element.name() + "> is closed by </" + std::string(closing) + ">");
                }
                skipSpace();
                if (next() != '>')
                    fail(atEnd() ? "the text ends inside a tag" : "expected the end of the tag");
                ++position;

                if (!element.children().empty() && std::all_of(content.begin(), content.end(), isSpace))
                    content.clear();
                element.setText(std::move(content));

                Element done = std::move(element);
                open.pop_back();
                if (open.empty())
                    return done;
                open.back().element.children().push_back(std::move(done));
            }
            else if (startsWith("<!--"))
                comment();
            else if (startsWith("<![CDATA["))
            {
                position += 9;
                const auto start = position;
                const auto inside = through("]]>", "the text ends inside a CDATA section");
                const auto end = position;
                position = start;
                characters(content, true, inside.size());
                position = end;
            }
            else if (startsWith("<?"))
                instruction();
            else if (startsWith("<!"))
                fail("a declaration can only come before the document's element");
            else
            {
                if (open.size() >= maximumDepth)
                    fail("elements are nested more than " + std::to_string(maximumDepth) + " deep");

                // Adding to the open elements may move them, so the loop finds the element and its text again.
                if (auto [child, closed] = startTag(); closed)
                    element.children().push_back(std::move(child));
                else
                    open.push_back({std::move(child), {}});
            }
        }
    }

    // Plain text up to the next markup or reference, or, for a CDATA section, the given number of bytes, with line
    // breaks read as line feeds.
    void characters(std::string& out, bool cdata, std::size_t count = 0)
    {
        const auto end = cdata ? position + count : text.size();
        auto runStart = position;

        while (position < end)
        {
            const auto c = static_cast<unsigned char>(text[position]);

            if (!cdata && (c == '<' || c == '&'))
                break;

            if (c == '\r')
            {
                out.append(text.substr(runStart, position - runStart));
                out += '\n';
                ++position;
                if (position < end && text[position] == '\n')
                    ++position;
                runStart = position;
                continue;
            }

            if (c < 0x20 && c != '\t' && c != '\n')
                fail("a control character can't be in XML text, except as a character reference");

            ++position;
        }

        out.append(text.substr(runStart, position - runStart));
    }

    // An attribute's value up to the closing quote, its spaces, tabs and line breaks each read as a space, as XML says.
    std::string attributeValue(char quote)
    {
        std::string out;

        for (;;)
        {
            if (atEnd())
                fail("the text ends inside an attribute's value");

            const auto c = static_cast<unsigned char>(text[position]);

            if (c == static_cast<unsigned char>(quote))
            {
                ++position;
                return out;
            }
            if (c == '&')
                reference(out);
            else if (c == '<')
                fail("an attribute's value can't hold <, except as &lt;");
            else if (c == '\r' || c == '\n' || c == '\t')
            {
                out += ' ';
                ++position;
                if (c == '\r' && next() == '\n')
                    ++position;
            }
            else if (c < 0x20)
                fail("a control character can't be in XML text, except as a character reference");
            else
            {
                out += static_cast<char>(c);
                ++position;
            }
        }
    }

    void reference(std::string& out)
    {
        const auto start = position;
        ++position;   // the &

        if (next() == '#')
        {
            ++position;
            const bool hex = next() == 'x';
            if (hex)
                ++position;

            char32_t code = 0;
            std::size_t digits = 0;

            for (; !atEnd() && next() != ';'; ++position, ++digits)
            {
                const char c = next();
                unsigned digit = 0;
                if (c >= '0' && c <= '9')
                    digit = static_cast<unsigned>(c - '0');
                else if (hex && c >= 'a' && c <= 'f')
                    digit = static_cast<unsigned>(c - 'a' + 10);
                else if (hex && c >= 'A' && c <= 'F')
                    digit = static_cast<unsigned>(c - 'A' + 10);
                else
                    break;

                code = static_cast<char32_t>(code * (hex ? 16u : 10u) + digit);
                if (code > 0x10FFFF)
                {
                    position = start;
                    fail("a character reference names no character");
                }
            }

            if (digits == 0 || next() != ';')
            {
                position = start;
                fail("a character reference is written wrongly");
            }
            if (code >= 0xD800 && code <= 0xDFFF)
            {
                position = start;
                fail("a character reference names no character");
            }

            ++position;
            utf8::append(out, code);
            return;
        }

        const auto end = text.find(';', position);
        const auto entity = end == std::string_view::npos || end - position > 8 ? std::string_view()
                                                                                : text.substr(position, end - position);
        const char* replacement = entity == "lt"     ? "<"
                                  : entity == "gt"   ? ">"
                                  : entity == "amp"  ? "&"
                                  : entity == "apos" ? "'"
                                  : entity == "quot" ? "\""
                                                     : nullptr;

        if (replacement == nullptr)
        {
            position = start;
            if (nameLength(text, start + 1) == 0)
                fail("an ampersand must start a reference, as &amp; does");
            fail("&" + std::string(text.substr(start + 1, nameLength(text, start + 1))) +
                 "; isn't an entity XML defines, and a document's own entities aren't read");
        }

        out += replacement;
        position = end + 1;
    }
};
}

Element::Element(std::string_view name) : tag(checkedName(name)) {}

void Element::rename(std::string_view name)
{
    tag = checkedName(name);
}

const std::string* Element::attribute(std::string_view name) const noexcept
{
    for (const auto& a : attrs)
        if (a.name == name)
            return &a.value;
    return nullptr;
}

std::string Element::attributeOr(std::string_view name, std::string_view fallback) const
{
    const auto* value = attribute(name);
    return std::string(value != nullptr ? std::string_view(*value) : fallback);
}

void Element::setAttribute(std::string_view name, std::string_view value)
{
    for (auto& a : attrs)
    {
        if (a.name == name)
        {
            a.value = value;
            return;
        }
    }
    attrs.push_back({checkedName(name), std::string(value)});
}

bool Element::removeAttribute(std::string_view name)
{
    return std::erase_if(attrs, [&](const Attribute& a) { return a.name == name; }) != 0;
}

Element* Element::child(std::string_view name) noexcept
{
    const auto found = std::find_if(kids.begin(), kids.end(), [&](const Element& e) { return e.tag == name; });
    return found != kids.end() ? &*found : nullptr;
}

const Element* Element::child(std::string_view name) const noexcept
{
    return const_cast<Element*>(this)->child(name);
}

Element& Element::add(Element element)
{
    return kids.emplace_back(std::move(element));
}

std::string write(const Element& root, Layout layout, bool declaration)
{
    std::string out;
    if (declaration)
        out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    writeElement(out, root, layout, 0);
    if (layout == Layout::pretty)
        out += '\n';
    return out;
}

ParseError::ParseError(const std::string& reason, std::size_t line, std::size_t column)
    : std::runtime_error("XML line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + reason),
      why(reason), where{line, column}
{
}

Element parse(std::string_view text)
{
    return Parser(text).document();
}

std::optional<Element> tryParse(std::string_view text)
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
