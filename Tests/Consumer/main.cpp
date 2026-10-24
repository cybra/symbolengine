#include <symbolengine/core.h>
#include <symbolengine/files.h>

#include <iostream>
#include <vector>

int main()
{
    using namespace symbolengine;

    const auto value = json::parse(R"({"name": "consumer", "rate": 48000})");
    const auto written = json::write(value);

    // A second of a WAV, written and read back.
    const auto file = paths::systemFolder(paths::Folder::temp) / "symbolengine-consumer.wav";
    const std::vector<float> silence(48000);
    const float* channels[1]{silence.data()};
    files::createAudio(file, {"WAV", 48000, 1, 24, false, 0, {}})->write(channels, 48000);
    const auto frames = files::openAudio(file)->info().frames;
    std::filesystem::remove(file);

    const bool works = written == R"({"name":"consumer","rate":48000})" && frames == 48000;
    std::cout << written << ", " << frames << " frames" << (works ? " works" : " is wrong") << '\n';
    return works ? 0 : 1;
}
