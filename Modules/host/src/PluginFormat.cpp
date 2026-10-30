#include "symbolengine/host/PluginFormat.h"

#include <symbolengine/core/System.h>

#include <vector>

namespace symbolengine
{

bool PluginFormat::runs(std::string_view architecture) const
{
    return architecture == system::architecture();
}

std::optional<std::filesystem::path> PluginFormat::changedAfterSigning(const std::filesystem::path&) const
{
    return std::nullopt;
}

PluginIdentity PluginIdentity::audioUnit(std::uint32_t type, std::uint32_t subtype, std::uint32_t manufacturer)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string id;
    for (const auto code : {type, subtype, manufacturer})
    {
        if (!id.empty())
            id += '-';
        for (int shift = 28; shift >= 0; shift -= 4)
            id += digits[(code >> shift) & 0xF];
    }
    return {"AudioUnit", id};
}

namespace
{

// A version's pieces: runs of digits, compared as numbers, and runs of letters, compared as text without case. Every
// other character only separates them.
struct Piece
{
    bool number = false;
    std::string_view text;
};

enum class Character
{
    digit,
    letter,
    separator,
};

Character kindOf(char c)
{
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= '0' && byte <= '9')
        return Character::digit;
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte >= 0x80)
        return Character::letter;
    return Character::separator;
}

std::vector<Piece> piecesOf(std::string_view version)
{
    std::vector<Piece> pieces;
    std::size_t start = 0;
    while (start < version.size())
    {
        const auto kind = kindOf(version[start]);
        if (kind == Character::separator)
        {
            ++start;
            continue;
        }
        auto end = start + 1;
        while (end < version.size() && kindOf(version[end]) == kind)
            ++end;
        pieces.push_back({kind == Character::digit, version.substr(start, end - start)});
        start = end;
    }
    return pieces;
}

int sign(int value)
{
    return (value > 0) - (value < 0);
}

// Digits of any length, by their value.
int compareNumbers(std::string_view a, std::string_view b)
{
    while (a.size() > 1 && a.front() == '0')
        a.remove_prefix(1);
    while (b.size() > 1 && b.front() == '0')
        b.remove_prefix(1);
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return sign(a.compare(b));
}

int compareText(std::string_view a, std::string_view b)
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return static_cast<unsigned char>(lower(a[i])) < static_cast<unsigned char>(lower(b[i])) ? -1 : 1;
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

}

int compareVersions(std::string_view a, std::string_view b)
{
    const auto left = piecesOf(a);
    const auto right = piecesOf(b);
    std::size_t i = 0;
    for (; i < left.size() && i < right.size(); ++i)
    {
        const auto& one = left[i];
        const auto& other = right[i];
        if (one.number != other.number)
            return one.number ? 1 : -1;   // 1.0.1 is newer than 1.0beta
        if (const int order = one.number ? compareNumbers(one.text, other.text) : compareText(one.text, other.text))
            return order;
    }

    // What the longer one has after: zeros say nothing, a number makes it newer, and text makes it older, as "beta"
    // after "1.0" does.
    const auto rest = [i](const std::vector<Piece>& longer) {
        for (auto j = i; j < longer.size(); ++j)
            if (!longer[j].number || longer[j].text.find_first_not_of('0') != std::string_view::npos)
                return longer[j].number ? 1 : -1;
        return 0;
    };
    if (i < left.size())
        return rest(left);
    if (i < right.size())
        return -rest(right);
    return 0;
}

}
