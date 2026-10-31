#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

/** The gzip format (RFC 1952), whole files held in memory. */
namespace symbolengine::gzip
{

/** The bytes as a gzip file, compressed as far as the format's method goes. */
std::string compress(std::string_view bytes);

/** What a gzip file holds, its checksum and length checked; nothing for anything else, one cut short or changed, or
    one holding more than the limit, as a few megabytes can unpack to gigabytes: 1 GB unless given. */
std::optional<std::string> decompress(std::string_view file, std::size_t limit = std::size_t{1} << 30);
}
