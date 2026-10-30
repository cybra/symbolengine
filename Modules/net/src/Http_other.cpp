#include "symbolengine/net/Http.h"

namespace symbolengine::http
{

bool available()
{
    return false;
}

Answer get(const Request&, const std::function<bool(int status, std::string_view piece)>&)
{
    return {0, "This system can't fetch from web servers yet."};
}

}
