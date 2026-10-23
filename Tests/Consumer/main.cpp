#include <symbolengine/core.h>

#include <iostream>

int main()
{
    using namespace symbolengine;

    const auto value = json::parse(R"({"name": "consumer", "rate": 48000})");
    const auto written = json::write(value);
    const bool works =
        written == R"({"name":"consumer","rate":48000})" && paths::systemFolder(paths::Folder::temp).is_absolute();

    std::cout << written << (works ? " works" : " is wrong") << '\n';
    return works ? 0 : 1;
}
