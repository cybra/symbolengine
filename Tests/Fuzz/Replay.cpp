// Runs a fuzz target once over each file named or in each folder named, where the build has no libFuzzer to call it, so
// what the fuzzers found stays a test on every system. libFuzzer's own options, which start with a dash, are passed over,
// so one command line serves both.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace
{
int ran = 0;

void runOn(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    ++ran;
}
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument.starts_with("-"))
            continue;

        if (std::filesystem::is_directory(argument))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(argument))
                if (entry.is_regular_file())
                    runOn(entry.path());
        }
        else
        {
            runOn(argument);
        }
    }

    std::printf("Ran on %d files.\n", ran);

    // Running on nothing would pass without having checked anything.
    return ran > 0 ? 0 : 1;
}
