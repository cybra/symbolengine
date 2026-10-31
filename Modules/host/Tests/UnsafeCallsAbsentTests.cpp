// A program that isn't linked to what watches for unsafe calls, as the host's other tests aren't. On Windows it needs
// no such thing.

#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>

#include "../src/UnsafeCalls.h"
#include "TestMainThread.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
    #include <dlfcn.h>
#endif

using namespace symbolengine;
using namespace std::chrono_literals;

TEST_CASE("What watches for unsafe calls can't, loaded after a program has started, and a host asked to is unharmed")
{
#if defined(_WIN32)
    CHECK(PluginHost::canWatchUnsafeCalls());
#else
    #if defined(SYMBOLENGINE_UNSAFE_CALLS_LIBRARY)
    REQUIRE(dlopen(SYMBOLENGINE_UNSAFE_CALLS_LIBRARY, RTLD_NOW | RTLD_GLOBAL) != nullptr);
    REQUIRE(dlsym(RTLD_DEFAULT, "symbolengine_unsafe_calls_link") != nullptr);
    #endif
    CHECK_FALSE(PluginHost::canWatchUnsafeCalls());
#endif

    TestMainThread main;
    FakePluginFormat fake{main.isCurrent(), main.post()};
    PluginHost::Options options{main.post(), main.isCurrent(), 2000ms, nullptr};
    options.watchUnsafeCalls = true;
    PluginHost host{options};
    const auto plugin = host.make(fake, FakePluginFormat::describe("any"));
    REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    REQUIRE(plugin->prepare({48000.0, 512, false}));
    std::vector<float> left(512), right(512);
    float* channels[] = {left.data(), right.data()};
    plugin->process(channels, 2, 512);
    CHECK(plugin->timing().blocks == 1);
#if !defined(_WIN32)
    CHECK(plugin->unsafeCalls().empty());
#endif
}

// What a plugin's record does with what it is told, with nothing watching for real, so this runs under the sanitizers,
// whose own interception of these calls keeps what really watches from being built with them.
TEST_CASE("A plugin's record keeps each place once and in order, counts what it can't keep, and can be read meanwhile")
{
    const unsafeCalls::Link link{[](unsafeCalls::Saw) {}, [](void*) {}, [] {}, [] {}};
    std::atomic<int> told{0};
    unsafeCalls::Record record(link, {}, [](void* context) { ++*static_cast<std::atomic<int>*>(context); }, &told);

    // A function's name, by where it is held, tells one place from another.
    std::array<std::string, 100> names;

    for (std::size_t i = 0; i < names.size(); ++i)
        names[i] = "function " + std::to_string(i);

    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done)
            for (const auto& call : record.calls())
                CHECK_FALSE(call.function.empty());
    });

    for (std::int64_t block = 3; block < 6; ++block)
    {
        record.enter("process", block);

        for (const auto& name : names)
            unsafeCalls::Record::saw(&record, name.c_str(), unsafeCalls::lockCall, nullptr);

        record.leave();
    }

    done = true;
    reader.join();
    const auto calls = record.calls();
    REQUIRE(calls.size() == 64);
    CHECK(told == 64);
    CHECK(record.notListed() == 3 * 36);

    for (std::size_t i = 0; i < calls.size(); ++i)
    {
        CHECK(calls[i].function == names[i]);
        CHECK(calls[i].kind == PluginUnsafeCall::Kind::lock);
        CHECK(calls[i].step == "process");
        CHECK(calls[i].times == 3);
        CHECK(calls[i].blocks == 3);
        CHECK(calls[i].firstBlock == 3);
        CHECK(calls[i].lastBlock == 5);
        CHECK_FALSE(calls[i].backtrace.empty());
    }
}
