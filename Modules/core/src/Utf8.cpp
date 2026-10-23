#include "Utf8.h"

namespace symbolengine::utf8
{

namespace
{
// The WHATWG UTF-8 decoder, one byte at a time. Given the start of a sequence, it says how long the sequence is
// when it is well formed, or how many bytes make up its maximal broken part when it isn't.
struct Sequence
{
    std::size_t length;
    bool valid;
};

Sequence sequenceAt(std::string_view text, std::size_t start) noexcept
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char first = byte(start);

    if (first < 0x80)
        return {1, true};

    std::size_t needed = 0;
    unsigned char lower = 0x80, upper = 0xBF;

    if (first >= 0xC2 && first <= 0xDF)
        needed = 1;
    else if (first >= 0xE0 && first <= 0xEF)
    {
        needed = 2;
        if (first == 0xE0)
            lower = 0xA0;
        if (first == 0xED)
            upper = 0x9F;
    }
    else if (first >= 0xF0 && first <= 0xF4)
    {
        needed = 3;
        if (first == 0xF0)
            lower = 0x90;
        if (first == 0xF4)
            upper = 0x8F;
    }
    else
        return {1, false};

    for (std::size_t seen = 0; seen < needed; ++seen)
    {
        const std::size_t i = start + 1 + seen;
        if (i >= text.size() || byte(i) < lower || byte(i) > upper)
            return {1 + seen, false};
        lower = 0x80;
        upper = 0xBF;
    }

    return {1 + needed, true};
}
}

std::size_t sequenceLength(std::string_view text, std::size_t at) noexcept
{
    const auto sequence = sequenceAt(text, at);
    return sequence.valid ? sequence.length : 0;
}

std::size_t firstInvalid(std::string_view text) noexcept
{
    std::size_t i = 0;
    while (i < text.size())
    {
        if (static_cast<unsigned char>(text[i]) < 0x80)
        {
            ++i;
            continue;
        }
        const auto sequence = sequenceAt(text, i);
        if (!sequence.valid)
            return i;
        i += sequence.length;
    }
    return text.size();
}

void appendReplacingInvalid(std::string& destination, std::string_view text)
{
    std::size_t i = 0, runStart = 0;
    while (i < text.size())
    {
        if (static_cast<unsigned char>(text[i]) < 0x80)
        {
            ++i;
            continue;
        }
        const auto sequence = sequenceAt(text, i);
        if (!sequence.valid)
        {
            destination.append(text.substr(runStart, i - runStart));
            destination.append(replacementCharacter);
            runStart = i + sequence.length;
        }
        i += sequence.length;
    }
    destination.append(text.substr(runStart));
}

void append(std::string& destination, char32_t codePoint)
{
    if (codePoint < 0x80)
        destination += static_cast<char>(codePoint);
    else if (codePoint < 0x800)
    {
        destination += static_cast<char>(0xC0 | (codePoint >> 6));
        destination += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
        destination.append(replacementCharacter);
    else if (codePoint < 0x10000)
    {
        destination += static_cast<char>(0xE0 | (codePoint >> 12));
        destination += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        destination += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else if (codePoint <= 0x10FFFF)
    {
        destination += static_cast<char>(0xF0 | (codePoint >> 18));
        destination += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        destination += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        destination += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else
        destination.append(replacementCharacter);
}

}
