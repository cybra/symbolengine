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
    CHECK(firedAt - began >= std::chrono::milliseconds(150));
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
