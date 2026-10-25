#include <symbolengine/core/LockFreeFifo.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using namespace symbolengine;

TEST_CASE("LockFreeFifo holds exactly its capacity")
{
    LockFreeFifo<int> fifo(5);
    CHECK(fifo.capacity() == 5);
    CHECK(fifo.writable() == 5);

    const int items[] = {1, 2, 3, 4, 5, 6, 7};
    CHECK(fifo.write(items, 7) == 5);
    CHECK(fifo.readable() == 5);
    CHECK(fifo.writable() == 0);
    CHECK_FALSE(fifo.push(8));

    int out[7]{};
    CHECK(fifo.read(out, 7) == 5);
    CHECK(out[0] == 1);
    CHECK(out[4] == 5);
    CHECK(fifo.readable() == 0);
    int one = 0;
    CHECK_FALSE(fifo.pop(one));
}

TEST_CASE("LockFreeFifo keeps items in order as it goes round")
{
    LockFreeFifo<int> fifo(7);
    int next = 0, expected = 0;

    // Writes of three and reads of two go round the ring many times, each read giving the next items in order.
    for (int round = 0; round < 100; ++round)
    {
        const int in[3] = {next, next + 1, next + 2};
        next += static_cast<int>(fifo.write(in, 3));

        int out[2]{};
        const auto got = fifo.read(out, 2);
        for (std::size_t i = 0; i < got; ++i)
            REQUIRE(out[i] == expected++);
    }

    int rest[8]{};
    const auto got = fifo.read(rest, 8);
    for (std::size_t i = 0; i < got; ++i)
        REQUIRE(rest[i] == expected++);
    CHECK(expected == next);

    fifo.reset();
    CHECK(fifo.readable() == 0);
}

TEST_CASE("LockFreeFifo passes chunks of any size between two threads with every item in order")
{
    // A writing thread and a reading thread, each taking chunks of random sizes, neither waiting on the other but by
    // trying again: every number arrives once, in order.
    constexpr std::uint32_t total = 2'000'000;
    LockFreeFifo<std::uint32_t> fifo(1021);

    std::thread writer([&] {
        std::minstd_rand random(1);
        std::vector<std::uint32_t> chunk(300);
        std::uint32_t next = 0;
        while (next < total)
        {
            const auto size = std::min<std::uint32_t>(1 + random() % 300, total - next);
            for (std::uint32_t i = 0; i < size; ++i)
                chunk[i] = next + i;
            next += static_cast<std::uint32_t>(fifo.write(chunk.data(), size));
        }
    });

    std::minstd_rand random(2);
    std::vector<std::uint32_t> chunk(300);
    std::uint32_t expected = 0;
    bool inOrder = true;
    while (expected < total)
    {
        const auto got = fifo.read(chunk.data(), 1 + random() % 300);
        for (std::size_t i = 0; i < got; ++i)
            inOrder = inOrder && chunk[i] == expected++;
    }
    writer.join();

    CHECK(inOrder);
    CHECK(expected == total);
    CHECK(fifo.readable() == 0);
}
