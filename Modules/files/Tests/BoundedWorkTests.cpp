#include "BoundedWork.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using symbolengine::detail::BoundedWork;
using namespace std::chrono_literals;

TEST_CASE("Work that finishes gives its answer, on a thread of its own, however many times")
{
    BoundedWork work;
    const auto here = std::this_thread::get_id();
    for (int i = 0; i < 100; ++i)
    {
        std::thread::id there;
        REQUIRE(work.run([&] { there = std::this_thread::get_id(); }, 5s));
        CHECK(there != here);
    }
    CHECK_FALSE(work.stuck());
}

TEST_CASE("Work that never comes back is given up on within its limit, and nothing more is taken")
{
    auto letGo = std::make_shared<std::atomic<bool>>(false);
    auto gone = std::make_shared<std::atomic<bool>>(false);
    {
        BoundedWork work;
        const auto began = std::chrono::steady_clock::now();
        const bool finished = work.run(
            [letGo, gone] {
                while (!letGo->load())
                    std::this_thread::sleep_for(1ms);
                gone->store(true);
            },
            100ms);
        CHECK_FALSE(finished);
        CHECK(std::chrono::steady_clock::now() - began >= 100ms);
        CHECK(std::chrono::steady_clock::now() - began < 3s);
        CHECK(work.stuck());

        bool ran = false;
        CHECK_FALSE(work.run([&] { ran = true; }, 5s));
        CHECK_FALSE(ran);

        // Letting go of it waits for nothing.
    }
    CHECK_FALSE(gone->load());

    // It may come back after all, with what it holds still there.
    letGo->store(true);
    for (int i = 0; i < 2000 && !gone->load(); ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(gone->load());
}

TEST_CASE("Work that keeps moving has as long as it needs, the limit running from when it last moved")
{
    BoundedWork work;
    std::atomic<std::uint64_t> moved{0};
    CHECK(work.run(
        [&] {
            for (int i = 0; i < 12; ++i)
            {
                std::this_thread::sleep_for(50ms);
                ++moved;
            }
        },
        300ms, &moved));
    CHECK_FALSE(work.stuck());
}

TEST_CASE("Work's own hold on what it uses goes on its thread, before the answer")
{
    BoundedWork work;
    auto thing = std::make_shared<int>(1);
    std::weak_ptr<int> watched = thing;
    REQUIRE(work.run([held = std::move(thing)] {}, 5s));
    CHECK(watched.expired());
}
