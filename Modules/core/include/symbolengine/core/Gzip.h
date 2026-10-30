#pragma once

#include <optional>
#include <string>
#include <string_view>

/** The gzip format (RFC 1952), whole files held in memory. */
namespace symbolengine::gzip
{

/** The bytes as a gzip file, compressed as far as the format's method goes. */
std::string compress(std::string_view bytes);

/** What a gzip file holds, its checksum and length checked; nothing for anything else, or one cut short or changed. */
std::optional<std::string> decompress(std::string_view file);

}
