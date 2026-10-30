#include "symbolengine/net/Http.h"

#include "symbolengine/core/Paths.h"
#include "symbolengine/core/Text.h"

#include <string>

namespace symbolengine::http
{
namespace
{

bool isPlain(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' ||
           character == '~';
}

int hexValue(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

}

std::string escaped(std::string_view part)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;

    for (const auto character : part)
    {
        if (isPlain(character))
        {
            out += character;
            continue;
        }

        const auto byte = static_cast<unsigned char>(character);
        out += '%';
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }

    return out;
}

std::string fileAddress(const std::filesystem::path& file)
{
    auto text = paths::toUtf8(file.generic_u8string().empty() ? file : std::filesystem::path(file.generic_u8string()));
    std::string address = "file://";

    if (text.empty() || text[0] != '/')
        address += '/';

    for (const auto piece : text::split(text, "/"))
    {
        // A Windows drive's colon stays as it is in the first part.
        address += address.size() > 8 || !piece.empty() ? escaped(piece) : std::string();
        address += '/';
    }

    address.pop_back();
    return text::replaceAll(address, "%3A", ":");
}

std::optional<std::filesystem::path> fileAt(std::string_view address)
{
    constexpr std::string_view scheme = "file://";

    if (!address.starts_with(scheme))
        return std::nullopt;

    address.remove_prefix(scheme.size());
    std::string path;

    for (std::size_t i = 0; i < address.size(); ++i)
    {
        if (address[i] == '%' && i + 2 < address.size() && hexValue(address[i + 1]) >= 0 &&
            hexValue(address[i + 2]) >= 0)
        {
            path += static_cast<char>(hexValue(address[i + 1]) * 16 + hexValue(address[i + 2]));
            i += 2;
        }
        else
        {
            path += address[i];
        }
    }

#if defined(_WIN32)
    // /C:/Users is C:/Users.
    if (path.size() > 2 && path[0] == '/' && path[2] == ':')
        path.erase(0, 1);
#endif

    return paths::fromUtf8(path);
}

}
