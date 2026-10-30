#include <symbolengine/ui/MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

using namespace symbolengine;

TEST_CASE("The message loop runs what any thread posts, in order, and what is timed once its time has come")
{
    ui::messageLoop::initialize();
    CHECK(ui::messageLoop::isMessageThread());

    std::vector<int> order;
    bool otherThreadIsMessageThread = true;

    // Posted from another thread before the loop runs: queued until it does, in the order posted.
    std::thread other([&] {
        otherThreadIsMessageThread = ui::messageLoop::isMessageThread();
        ui::messageLoop::callAsync([&] { order.push_back(1); });
        ui::messageLoop::callAsync([&] { order.push_back(2); });
    });
    other.join();
    CHECK_FALSE(otherThreadIsMessageThread);

    // The later of two timed calls was asked for first, and the earlier one still comes first.
    const auto began = std::chrono::steady_clock::now();
    auto firedAt = began;
    ui::messageLoop::callAfter(150, [&] {
        firedAt = std::chrono::steady_clock::now();
        order.push_back(4);
        ui::messageLoop::stop();
    });
    ui::messageLoop::callAfter(30, [&] { order.push_back(3); });

    ui::messageLoop::run();

    CHECK(order == std::vector<int>{1, 2, 3, 4});
    // Windows' timers tick about every 16 ms, so one can come that much before its time.
    CHECK(firedAt - began >= std::chrono::milliseconds(130));
}

TEST_CASE("A call timed from inside the loop waits its time, and what is posted meanwhile runs before it")
{
    ui::messageLoop::initialize();
    std::vector<int> order;

    ui::messageLoop::callAsync([&] {
        ui::messageLoop::callAfter(60, [&] {
            order.push_back(3);
            ui::messageLoop::stop();
        });
        ui::messageLoop::callAsync([&] { order.push_back(2); });
        order.push_back(1);
    });

    ui::messageLoop::run();
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("A call that waits on the message thread gets what is given to callSoon, even inside a call from callAsync")
{
    ui::messageLoop::initialize();

    bool held = false, late = true, stoppedWaiting = false;
    std::vector<int> order;
    std::thread other;
    std::chrono::steady_clock::duration waited{};

    ui::messageLoop::callAsync([&] {
        // Given from another thread while this call is under way, as a plugin's completion is.
        other = std::thread([&] {
            for (int i = 1; i <= 3; ++i)
                ui::messageLoop::callSoon([&order, i] { order.push_back(i); });
        });

        held = ui::messageLoop::runUntil([&] { return order.size() == 3; }, std::chrono::seconds(10));

        // The limit ends a wait for what never comes, in about the time it says.
        const auto began = std::chrono::steady_clock::now();
        late = ui::messageLoop::runUntil([] { return false; }, std::chrono::milliseconds(100));
        waited = std::chrono::steady_clock::now() - began;
        stoppedWaiting = true;
        ui::messageLoop::stop();
    });
    ui::messageLoop::run();
    other.join();

    CHECK(held);
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK(stoppedWaiting);
    CHECK_FALSE(late);
    CHECK(waited >= std::chrono::milliseconds(100));
    CHECK(waited < std::chrono::seconds(5));
}

TEST_CASE("callSoon runs from any thread while the loop runs, on the message thread")
{
    ui::messageLoop::initialize();

    bool onMessageThread = false;
    std::thread other([&] {
        ui::messageLoop::callSoon([&] {
            onMessageThread = ui::messageLoop::isMessageThread();
            ui::messageLoop::stop();
        });
    });
    ui::messageLoop::run();
    other.join();
    CHECK(onMessageThread);
}
