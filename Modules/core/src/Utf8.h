#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace symbolengine::utf8
{

inline constexpr std::string_view replacementCharacter = "\xEF\xBF\xBD";

/** The length of the well-formed UTF-8 sequence starting at this byte, or 0 if it is broken there. */
std::size_t sequenceLength(std::string_view text, std::size_t at) noexcept;

/** Where the first byte that isn't part of well-formed UTF-8 is, or the text's size if there is none. */
std::size_t firstInvalid(std::string_view text) noexcept;

/** Appends the text, each maximal broken part of a UTF-8 sequence as U+FFFD, as the WHATWG Encoding standard
    decodes. */
void appendReplacingInvalid(std::string& destination, std::string_view text);

/** Appends a code point as UTF-8; anything that isn't a Unicode scalar value becomes U+FFFD. */
void append(std::string& destination, char32_t codePoint);

}
