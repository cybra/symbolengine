#include <symbolengine/core/Xml.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace symbolengine;
using namespace std::string_view_literals;

namespace
{
// Where parsing the text fails, as "line:column reason".
std::string failure(std::string_view text)
{
    try
    {
        xml::parse(text);
        return "parsed";
    }
    catch (const xml::ParseError& error)
    {
        return std::to_string(error.line()) + ":" + std::to_string(error.column()) + " " + error.reason();
    }
}

std::string nested(std::size_t depth)
{
    std::string text;
    for (std::size_t i = 0; i < depth; ++i)
        text += "<a>";
    for (std::size_t i = 0; i < depth; ++i)
        text += "</a>";
    return text;
}
}

TEST_CASE("XML reads elements, their attributes in order, the elements inside them and their text")
{
    const auto root = xml::parse(R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- a list -->
<list kind="takes" count='2'>
  <take name="First">Read &amp; kept</take>
  <take name="Second"/>
  <empty></empty>
</list>
)");
    CHECK(root.name() == "list");
    REQUIRE(root.attributes().size() == 2);
    CHECK(root.attributes()[0] == xml::Attribute{"kind", "takes"});
    CHECK(root.attributes()[1] == xml::Attribute{"count", "2"});
    CHECK(root.text().empty());
    REQUIRE(root.children().size() == 3);
    CHECK(root.children()[0].attributeOr("name", "") == "First");
    CHECK(root.children()[0].text() == "Read & kept");
    CHECK(root.children()[1].attributeOr("name", "") == "Second");
    CHECK(root.children()[1].children().empty());
    CHECK(root.children()[2].text().empty());
    CHECK(root.child("take") == &root.children()[0]);
    CHECK(root.child("missing") == nullptr);
    CHECK(root.attribute("missing") == nullptr);
    CHECK(root.attributeOr("missing", "fallback") == "fallback");
}

TEST_CASE("XML replaces the entities it defines and character references, and reads CDATA as text")
{
    const auto root =
        xml::parse("<a b=\"&lt;&gt;&amp;&apos;&quot;&#65;&#x42;&#x1F3A7;\">&#233;<![CDATA[<raw> & ]]&#]]></a>");
    CHECK(*root.attribute("b") == "<>&'\"AB\xF0\x9F\x8E\xA7");
    CHECK(root.text() == "\xC3\xA9<raw> & ]]&#");
}

TEST_CASE("XML reads line breaks as line feeds, and spaces in attributes as XML says")
{
    const auto root = xml::parse("<a b=\"one\r\ntwo\tthree\nfour\" c=\"&#10;&#9;&#13;\">x\r\ny\rz</a>");
    CHECK(*root.attribute("b") == "one two three four");
    CHECK(*root.attribute("c") == "\n\t\r");
    CHECK(root.text() == "x\ny\nz");
}

TEST_CASE("XML keeps text in an element of text, but not the layout between elements")
{
    const auto root = xml::parse("<a>\n  <b>  spaced  </b>\n  <c> </c>\n</a>");
    CHECK(root.text().empty());
    CHECK(root.children()[0].text() == "  spaced  ");
    CHECK(root.children()[1].text() == " ");

    const auto mixed = xml::parse("<p>Some <b>bold</b> text</p>");
    CHECK(mixed.text() == "Some  text");
    CHECK(mixed.children()[0].text() == "bold");
}

TEST_CASE("XML reads past a document type declaration, its own entities never used")
{
    const auto root = xml::parse(R"(<?xml version="1.0"?>
<!DOCTYPE lolz [
  <!ENTITY lol "lol">
  <!ENTITY lol2 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">
  <!-- a comment with a ] and a > -->
  <!ATTLIST a b CDATA "]>">
]>
<a>plain</a>)");
    CHECK(root.text() == "plain");

    CHECK(failure("<!DOCTYPE a [<!ENTITY lol \"lol\">]><a>&lol;</a>") ==
          "1:38 &lol; isn't an entity XML defines, and a document's own entities aren't read");
}

TEST_CASE("XML reads processing instructions and comments anywhere they may be, and skips a byte order mark")
{
    const auto root = xml::parse("\xEF\xBB\xBF<?xml version='1.0' encoding='utf-8' standalone='yes'?><?app go?><a><!-- "
                                 "x --><?app here?><b/></a><!-- after --> ");
    CHECK(root.name() == "a");
    CHECK(root.children().size() == 1);
}

TEST_CASE("XML says why and where a document isn't one it reads")
{
    CHECK(failure("") == "1:1 the text ends before the document's element");
    CHECK(failure("text") == "1:1 expected the document's element");
    CHECK(failure("<a>") == "1:4 the text ends inside <a>");
    CHECK(failure("<a></b>") == "1:4 <a> is closed by </b>");
    CHECK(failure("<a b=\"1\" b=\"2\"/>") == "1:10 the attribute b is given twice");
    CHECK(failure("<a b=1/>") == "1:6 an attribute's value must be in quotes");
    CHECK(failure("<a b=\"<\"/>") == "1:7 an attribute's value can't hold <, except as &lt;");
    CHECK(failure("<a b/>") == "1:5 expected an equals sign after the attribute's name");
    CHECK(failure("<a b=\"1\"c=\"2\"/>") == "1:9 expected a space, or the end of the tag");
    CHECK(failure("<1/>") == "1:2 expected an element's name");
    CHECK(failure("<a/><b/>") == "1:5 there is more after the document's element");
    CHECK(failure("<a>&nbsp;</a>") ==
          "1:4 &nbsp; isn't an entity XML defines, and a document's own entities aren't read");
    CHECK(failure("<a>&</a>") == "1:4 an ampersand must start a reference, as &amp; does");
    CHECK(failure("<a>&#xD800;</a>") == "1:4 a character reference names no character");
    CHECK(failure("<a>&#x110000;</a>") == "1:4 a character reference names no character");
    CHECK(failure("<a>&#;</a>") == "1:4 a character reference is written wrongly");
    CHECK(failure("<a>&#12a;</a>") == "1:4 a character reference is written wrongly");
    CHECK(failure("<a>\x01</a>") == "1:4 a control character can't be in XML text, except as a character reference");
    CHECK(failure("<a>\xC3</a>") == "1:4 the text isn't valid UTF-8");
    CHECK(failure("<a>\n<!-- open</a>") == "2:14 the text ends inside a comment");
    CHECK(failure("<a><![CDATA[x</a>") == "1:18 the text ends inside a CDATA section");
    CHECK(failure("<a><!DOCTYPE a></a>") == "1:4 a declaration can only come before the document's element");
    CHECK(failure("<a/><?xml version=\"1.0\"?>") == "1:7 the XML declaration must come first");
    CHECK(failure("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a/>") ==
          "1:1 the document says it is in ISO-8859-1, and only UTF-8 is read");
    CHECK(failure("\xFF\xFE<\0a\0/\0>\0"sv) == "1:1 the text isn't valid UTF-8");
    CHECK_FALSE(xml::tryParse("<a>"));
    CHECK(xml::tryParse("<a/>"));
}

TEST_CASE("XML refuses elements nested deeper than its limit")
{
    CHECK(failure(nested(xml::maximumDepth)) == "parsed");
    CHECK(failure(nested(xml::maximumDepth + 1)) ==
          "1:" + std::to_string(xml::maximumDepth * 3 + 1) + " elements are nested more than 512 deep");

    // One as deep as the limit is written, read, compared and let go of on every system's stack too.
    xml::Element deepest("a");
    for (std::size_t i = 1; i < xml::maximumDepth; ++i)
    {
        xml::Element outer("a");
        outer.add(std::move(deepest));
        deepest = std::move(outer);
    }
    CHECK(xml::parse(xml::write(deepest)) == deepest);
}

TEST_CASE("XML is written compact or laid out, with the declaration or without")
{
    xml::Element root("settings");
    root.setAttribute("version", "2");
    auto& value = root.add(xml::Element("value"));
    value.setAttribute("name", "window");
    value.setText("10 20");
    root.add(xml::Element("empty"));
    auto& group = root.add(xml::Element("group"));
    group.add(xml::Element("inner")).setAttribute("a", "b");

    CHECK(
        xml::write(root, xml::Layout::compact, false) ==
        R"(<settings version="2"><value name="window">10 20</value><empty/><group><inner a="b"/></group></settings>)");
    CHECK(xml::write(root) == R"(<?xml version="1.0" encoding="UTF-8"?>
<settings version="2">
  <value name="window">10 20</value>
  <empty/>
  <group>
    <inner a="b"/>
  </group>
</settings>
)");

    // An element with text and elements is written compact inside, so the layout doesn't become its text.
    xml::Element mixed("p");
    mixed.setText("text");
    mixed.add(xml::Element("b")).add(xml::Element("i"));
    CHECK(xml::write(mixed, xml::Layout::pretty, false) == "<p>text<b><i/></b></p>\n");
}

TEST_CASE("XML escapes what must be, so every string reads back as it was written")
{
    xml::Element root("a");
    root.setAttribute("b", "<&>\"'\t\n\r\x01");
    root.setText("<&>\"'\t\n\r\x01\xEF\xBF\xBE");
    CHECK(xml::write(root, xml::Layout::compact, false) ==
          "<a b=\"&lt;&amp;&gt;&quot;'&#9;&#10;&#13;&#1;\">&lt;&amp;&gt;\"'\t\n&#13;&#1;&#65534;</a>");

    xml::Element broken("a");
    broken.setAttribute("b", "x\xC3y");
    broken.setText("\xFF");
    CHECK(xml::write(broken, xml::Layout::compact, false) == "<a b=\"x\xEF\xBF\xBDy\">\xEF\xBF\xBD</a>");
}

TEST_CASE("XML elements keep attributes in their places, and refuse names XML doesn't allow")
{
    xml::Element e("caf\xC3\xA9");
    e.setAttribute("a", "1");
    e.setAttribute("b", "2");
    e.setAttribute("a", "3");
    REQUIRE(e.attributes().size() == 2);
    CHECK(e.attributes()[0] == xml::Attribute{"a", "3"});
    CHECK(e.removeAttribute("a"));
    CHECK_FALSE(e.removeAttribute("a"));
    CHECK(e.attributes() == std::vector<xml::Attribute>{{"b", "2"}});

    CHECK_THROWS_AS(xml::Element(""), std::invalid_argument);
    CHECK_THROWS_AS(xml::Element("1a"), std::invalid_argument);
    CHECK_THROWS_AS(xml::Element("a b"), std::invalid_argument);
    CHECK_THROWS_AS(xml::Element("a>"), std::invalid_argument);
    CHECK_THROWS_AS(e.setAttribute("-a", "1"), std::invalid_argument);
    CHECK_THROWS_AS(e.rename("a\xC3"), std::invalid_argument);
    CHECK_NOTHROW(e.rename("ns:a-b.c_\xC2\xB7"));
    CHECK(e.name() == "ns:a-b.c_\xC2\xB7");
}

TEST_CASE("XML written and read again is the same, for any element")
{
    std::mt19937 random(7);
    const auto pick = [&](int n) { return static_cast<int>(random() % static_cast<unsigned>(n)); };
    const std::vector<std::string> pieces = {"a",
                                             " ",
                                             "\t",
                                             "\n",
                                             "\r",
                                             "\r\n",
                                             "<",
                                             ">",
                                             "&",
                                             "\"",
                                             "'",
                                             "]]>",
                                             "\x01",
                                             "\x1F",
                                             std::string(1, '\0'),
                                             "\xC3\xA9",
                                             "\xF0\x9F\x8E\xA7",
                                             "\xEF\xBF\xBE",
                                             "&amp;",
                                             "&#10;"};
    const auto someText = [&] {
        std::string text;
        for (int i = pick(6); i > 0; --i)
            text += pieces[static_cast<std::size_t>(pick(static_cast<int>(pieces.size())))];
        return text;
    };

    std::function<xml::Element(int)> make = [&](int depth) {
        xml::Element e(pick(2) == 0 ? "e" : "n" + std::to_string(pick(100)));
        for (int i = pick(4); i > 0; --i)
            e.setAttribute("a" + std::to_string(pick(5)), someText());
        if (depth < 4)
            for (int i = pick(4); i > 0; --i)
                e.add(make(depth + 1));

        // Text that is only layout isn't kept beside elements, so it isn't made there.
        auto text = someText();
        if (!e.children().empty() && text.find_first_not_of(" \t\r\n") == std::string::npos)
            text.clear();
        e.setText(text);
        return e;
    };

    for (int round = 0; round < 300; ++round)
    {
        const auto element = make(0);
        for (const auto layout : {xml::Layout::compact, xml::Layout::pretty})
        {
            const auto written = xml::write(element, layout, round % 2 == 0);
            const auto read = xml::parse(written);
            REQUIRE(read == element);
            CHECK(xml::write(read, layout, round % 2 == 0) == written);
        }
    }
}

TEST_CASE("An element's many attributes take no longer than their number")
{
    std::string text = "<Root";
    constexpr int many = 200000;
    for (int i = 0; i < many; ++i)
        text += " a" + std::to_string(i) + "=\"1\"";
    text += "/>";

    const auto root = xml::parse(text);
    CHECK(root.attributes().size() == many);
    CHECK(root.attribute("a199999") != nullptr);

    // Given twice is still refused, past where any short list would be looked through.
    text.resize(text.size() - 2);
    text += " a100=\"2\"/>";
    CHECK_FALSE(xml::tryParse(text).has_value());
}
