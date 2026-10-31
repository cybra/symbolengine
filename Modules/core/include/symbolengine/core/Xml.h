#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/** Reading and writing XML 1.0 documents of data, in UTF-8: settings, lists, project and session files. */
namespace symbolengine::xml
{

/** A name and its value, the value as plain text with any references already replaced. */
struct Attribute
{
    std::string name;
    std::string value;

    bool operator==(const Attribute&) const = default;
};

/** An element: its name, its attributes in order, the elements inside it in order, and its text.

    An element's text is all the text directly inside it, joined, wherever it stood among the elements; it is kept as
    a whole, not in pieces between them. That is how documents of data use text, and it means a document that mixes
    text and elements as running prose does, as XHTML does, can be read but its text no longer interleaves with its
    elements. In an element that has elements inside it, text that is only spaces and line breaks is the document's
    layout and isn't kept. */
struct ParsedElement;

class Element
{
public:
    /** Throws std::invalid_argument if the name isn't one XML allows. */
    explicit Element(std::string_view name);

    const std::string& name() const noexcept { return tag; }
    void rename(std::string_view name);

    const std::vector<Attribute>& attributes() const noexcept { return attrs; }

    /** The attribute's value, or nullptr if there is no such attribute. */
    const std::string* attribute(std::string_view name) const noexcept;

    /** The attribute's value, or the fallback if there is no such attribute. */
    std::string attributeOr(std::string_view name, std::string_view fallback) const;

    /** Sets an attribute, keeping its place if it is there already and adding it at the end if not. Throws
        std::invalid_argument if the name isn't one XML allows. */
    void setAttribute(std::string_view name, std::string_view value);

    /** Removes an attribute, and says whether there was one. */
    bool removeAttribute(std::string_view name);

    /** The elements inside this one, which can be changed, added to and removed from directly. */
    std::vector<Element>& children() noexcept { return kids; }
    const std::vector<Element>& children() const noexcept { return kids; }

    /** The first element inside this one with this name, or nullptr if there is none. */
    Element* child(std::string_view name) noexcept;
    const Element* child(std::string_view name) const noexcept;

    /** Adds an element at the end, and gives the one added. */
    Element& add(Element element);

    const std::string& text() const noexcept { return content; }
    void setText(std::string text) { content = std::move(text); }

    /** Two elements are equal when their names, attributes in order, elements and text are. */
    bool operator==(const Element&) const = default;

private:
    friend struct ParsedElement;

    std::string tag;
    std::vector<Attribute> attrs;
    std::vector<Element> kids;
    std::string content;
};

enum class Layout
{
    compact,   // no spaces or line breaks between elements
    pretty     // each element on its own line, indented by two spaces for each level
};

/** Writes an element as an XML document, starting with <?xml version="1.0" encoding="UTF-8"?> on a line of its own
    unless the declaration is left out. An element with text and elements inside it is written compact inside, so its
    text reads back as it was.

    Every string reads back as it was written, including tabs and line breaks in attribute values, which are written
    as character references. Characters XML 1.0 doesn't allow, the control characters other than tab, line feed and
    carriage return, are written as character references too, as some writers do; this module reads them back, but a
    strict reader refuses them. Text that isn't valid UTF-8 has each broken sequence written as U+FFFD. */
std::string write(const Element& root, Layout layout = Layout::pretty, bool declaration = true);

/** Thrown when text isn't an XML document this module reads, saying why and where: the line and column, from 1,
    with the column counted in bytes. */
class ParseError : public std::runtime_error
{
public:
    ParseError(const std::string& reason, std::size_t line, std::size_t column);

    const std::string& reason() const noexcept { return why; }
    std::size_t line() const noexcept { return where.line; }
    std::size_t column() const noexcept { return where.column; }

private:
    std::string why;
    struct
    {
        std::size_t line, column;
    } where;
};

/** The deepest elements may nest, so that hostile text can't exhaust the stack. */
inline constexpr std::size_t maximumDepth = 512;

/** Reads an XML document in UTF-8 and gives its root element, throwing ParseError if the text isn't well formed. A
    byte order mark at the start is skipped, and the declaration, comments, processing instructions and a document
    type declaration are read past. The five entities XML defines and character references are replaced; a document
    type declaration's own entities aren't read, so text that uses them is refused, which also means a document can't
    make itself grow without bound as they let it. Line breaks are read as line feeds, as XML says. */
Element parse(std::string_view text);

/** Reads an XML document, or gives nothing if it isn't one. */
std::optional<Element> tryParse(std::string_view text);

}
