// The devices section's lessons for a stream, each a test against the fake system: its threads, the callback's
// contract, the timeline, losing a device, sleep and changes the system makes.

#include "Helpers.h"

#include <symbolengine/devices/AudioThread.h>
#include <symbolengine/devices/AudioThreadChecks.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <set>

using namespace symbolengine;
using Kind = AudioEvent::Kind;

namespace
{
struct Rig
{
    std::shared_ptr<FakeAudioSystem> system = std::make_shared<FakeAudioSystem>();
    FakeAudioDevice& device;
    Events events;

    explicit Rig(AudioDeviceInfo info = fakeDevice("interface")) : device(system->add(std::move(info))) {}

    std::unique_ptr<AudioStream> stream(AudioStreamRequest request = stereo(),
                                        std::function<void(AudioStream::Options&)> adjust = {})
    {
        auto options = quick(events);
        if (adjust)
            adjust(options);
        return std::make_unique<AudioStream>(system, device.info().id, request, options);
    }
};
}

// Threads

TEST_CASE("A stop returns within its bound when the system's stop never does, and the stream says it failed")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    rig.device.hangStop = true;
    const auto before = std::chrono::steady_clock::now();
    const auto error = stream->stop();
    const auto took = std::chrono::steady_clock::now() - before;

    CHECK(error);
    CHECK(took < 1500ms);
    CHECK(stream->state() == AudioStream::State::failed);
    CHECK(stream->start());

    rig.device.hangStop = false;
}

TEST_CASE("A cycle the system gives after its stop has returned reaches no callback")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    rig.device.callAfterStop = true;
    const auto blocksBeforeStop = recording.kept().size();
    REQUIRE(!stream->stop());
    std::this_thread::sleep_for(60ms);

    CHECK(recording.kept().size() == blocksBeforeStop);
    CHECK(recording.stops == 1);
    const auto order = recording.order();
    CHECK(order.back() == Recording::stoppedCall);
}

TEST_CASE("Events reach the program one at a time, in order, on the thread it chose")
{
    // The program's own thread, which runs what is posted to it in turn.
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    bool done = false;
    std::thread programThread([&] {
        std::unique_lock hold(lock);
        while (!done || !queue.empty())
        {
            wake.wait(hold, [&] { return done || !queue.empty(); });
            while (!queue.empty())
            {
                auto next = std::move(queue.front());
                queue.pop_front();
                hold.unlock();
                next();
                hold.lock();
            }
        }
    });

    const auto programId = programThread.get_id();

    Rig rig;
    auto stream = rig.stream(stereo(), [&](AudioStream::Options& options) {
        options.post = [&](std::function<void()> f) {
            {
                const std::lock_guard hold(lock);
                queue.push_back(std::move(f));
            }
            wake.notify_one();
        };
    });
    REQUIRE(!stream->start());

    // Dropouts at known places, while the system sends notifications from four threads at once.
    std::thread noise([&] { rig.system->notify(AudioSystemChange::devices, 50, 4); });
    for (int i = 0; i < 40; ++i)
        REQUIRE(rig.device.cycle({.jump = i % 4 == 3 ? 100 : 0}));
    noise.join();
    REQUIRE(rig.events.waitFor(Kind::dropout, 10));
    REQUIRE(!stream->stop());
    REQUIRE(rig.events.waitFor(Kind::stopped));
    stream.reset();

    {
        const std::lock_guard hold(lock);
        done = true;
    }
    wake.notify_one();
    programThread.join();

    CHECK_FALSE(rig.events.overlapped);
    for (const auto id : rig.events.deliveredOn())
        CHECK(id == programId);

    // Started first, the dropouts in the order they happened, stopped last.
    const auto all = rig.events.all();
    REQUIRE(all.size() >= 12);
    CHECK(all.front().kind == Kind::started);
    CHECK(all.back().kind == Kind::stopped);
    std::int64_t last = -1;
    for (const auto& event : rig.events.of(Kind::dropout))
    {
        if (event.direction != AudioDirection::input)
            continue;
        CHECK(event.position > last);
        CHECK(event.frames == 100);
        last = event.position;
    }
}

TEST_CASE("A system that calls back inside its start while holding its own lock never deadlocks")
{
    Rig rig;
    rig.device.cycleInsideStart = true;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));

    CHECK(!stream->start());
    CHECK(recording.kept().size() == 1);
    CHECK(!stream->stop());
}

TEST_CASE("A stream's own calls refuse to run from its callbacks")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    std::atomic<bool> refused{false};
    std::atomic<bool> tried{false};
    recording.onProcess = [&](const AudioBlock&) {
        if (!tried.exchange(true))
            refused = static_cast<bool>(stream->stop());
    };
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    CHECK(tried);
    CHECK(refused);
    CHECK(stream->state() == AudioStream::State::running);
}

TEST_CASE("Starting, stopping, changing and adding from eight threads at once, with losses, every call returns")
{
    Rig rig;
    rig.device.run({.speed = 0.0});
    auto stream = rig.stream();
    std::vector<std::unique_ptr<Recording>> recordings;
    for (int i = 0; i < 8; ++i)
        recordings.push_back(std::make_unique<Recording>(1024));

    std::atomic<int> slow{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            std::minstd_rand random(static_cast<unsigned>(t + 1));
            for (int round = 0; round < 150; ++round)
            {
                const auto began = std::chrono::steady_clock::now();
                switch (random() % 6)
                {
                    case 0: (void)stream->start(); break;
                    case 1: (void)stream->stop(); break;
                    case 2:
                    {
                        auto request = stereo();
                        request.sampleRate = random() % 2 == 0 ? 44100.0 : 48000.0;
                        (void)stream->change(request);
                        break;
                    }
                    case 3: (void)stream->add(*recordings[static_cast<std::size_t>(t)]); break;
                    case 4: (void)stream->remove(*recordings[static_cast<std::size_t>(t)]); break;
                    default:
                        if (t == 0 && round % 25 == 0)
                            rig.device.lose(AudioLoss::notAnswering);
                        break;
                }
                if (std::chrono::steady_clock::now() - began > 1500ms)
                    ++slow;
            }
        });
    for (auto& thread : threads)
        thread.join();

    CHECK(slow == 0);
    CHECK(stream->state() != AudioStream::State::failed);
    CHECK(rig.device.starts() > 10);

    // And it still plays.
    CHECK(!stream->start());
    CHECK(soon([&] { return stream->statistics().cycles > 0; }));
    (void)stream->stop();
    for (auto& recording : recordings)
    {
        (void)stream->remove(*recording);
        CHECK(recording->prepares == recording->stops);
    }
}

TEST_CASE("The audio thread allocates nothing through an hour of blocks")
{
    Rig rig;
    Recording recording(16);
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    const auto before = audioThread::allocations();
    // An hour at 48 kHz in blocks of 512, with dropouts and splits on the way.
    for (int i = 0; i < 337'500; ++i)
        REQUIRE(rig.device.cycle({.frames = static_cast<std::size_t>(i % 100 == 0 ? 1536 : 512),
                                  .jump = i % 1000 == 999 ? 64 : 0}));
    CHECK(audioThread::allocations() == before);
    CHECK(!stream->stop());
}

TEST_CASE("Denormal numbers made on the audio thread come out as zero")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    volatile float smallest = std::numeric_limits<float>::min();
    std::atomic<float> product{1.0f};
    recording.onProcess = [&](const AudioBlock&) { product = smallest * 0.1f; };
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    CHECK(product == 0.0f);
}

// The callback

TEST_CASE("Each callback starts before its first block and stops once after its last, through restarts")
{
    Rig rig;
    rig.device.run({.speed = 20.0});
    Recording first, second;
    auto stream = rig.stream();
    REQUIRE(!stream->add(first));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return first.frames > 10000; }));

    // One added and taken out from another thread while it runs, and the device changing rate under both.
    std::atomic<int> refused{0};
    std::thread other([&] {
        for (int i = 0; i < 20; ++i)
        {
            if (stream->add(second))
                ++refused;
            std::this_thread::sleep_for(2ms);
            if (stream->remove(second))
                ++refused;
        }
    });
    rig.device.changed([](AudioDeviceInfo& info) { info.currentRate = 44100.0; });
    REQUIRE(rig.events.waitFor(Kind::formatChanged));
    other.join();
    CHECK(refused == 0);
    REQUIRE(soon([&] { return first.rate == 44100.0; }));
    REQUIRE(!stream->stop());

    for (const Recording* recording : {&first, &second})
    {
        // Prepared, its blocks, stopped; again and again, never a block outside.
        const auto order = recording->order();
        bool open = false;
        for (const auto call : order)
        {
            if (call == Recording::prepared)
            {
                CHECK_FALSE(open);
                open = true;
            }
            else if (call == Recording::processed)
                CHECK(open);
            else
            {
                CHECK(open);
                open = false;
            }
        }
        CHECK_FALSE(open);
        CHECK(recording->prepares == recording->stops);
    }
    CHECK(second.prepares >= 20);
}

TEST_CASE("Blocks of any size the system gives reach the program within the announced size, every frame kept")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());

    std::minstd_rand random(3);
    std::int64_t given = 0;
    for (int i = 0; i < 2000; ++i)
    {
        const auto frames = static_cast<std::size_t>(random() % 1537);
        if (frames == 0)
            continue;
        REQUIRE(rig.device.cycle({.frames = frames}));
        given += static_cast<std::int64_t>(frames);
    }

    CHECK(recording.frames == given);
    CHECK(recording.largest <= 512);
    CHECK(recording.emptyBlocks == 0);
    CHECK(recording.maxBlock == 512);
}

TEST_CASE("A cycle of no frames is never passed on")
{
    Rig rig;
    rig.device.run({.speed = 0.0, .randomSizes = true, .cycles = 3000});
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return stream->statistics().cycles >= 3000; }));

    CHECK(recording.emptyBlocks == 0);
    CHECK(recording.largest <= 512);
    CHECK(recording.frames > 0);
}

TEST_CASE("Outputs arrive silent, and channels the program doesn't write stay silent")
{
    Rig rig(fakeDevice("eight", 8, 8));
    AudioStreamRequest request;
    request.inputChannels = {0, 1, 2, 3, 4, 5, 6, 7};
    request.outputChannels = {0, 1, 2, 3, 4, 5, 6, 7};
    Recording writesNothing;
    auto stream = rig.stream(request);
    REQUIRE(!stream->add(writesNothing));
    REQUIRE(!stream->start());
    rig.device.input = [](int, std::int64_t frame) { return std::sin(static_cast<float>(frame) * 0.01f); };
    REQUIRE(rig.device.cycle());

    const auto output = rig.device.lastOutput();
    REQUIRE(output.size() == 8);
    for (const auto& channel : output)
        for (const auto sample : channel)
            REQUIRE(sample == 0.0f);
}

TEST_CASE("Two callbacks' outputs are added together")
{
    Rig rig;
    Recording a, b;
    auto stream = rig.stream();
    const auto write = [](const AudioBlock& block) {
        for (int c = 0; c < block.outputChannels; ++c)
            for (int i = 0; i < block.frames; ++i)
                block.outputs[c][i] = 0.25f;
    };
    a.onProcess = write;
    b.onProcess = write;
    REQUIRE(!stream->add(a));
    REQUIRE(!stream->add(b));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    for (const auto& channel : rig.device.lastOutput())
        for (const auto sample : channel)
            REQUIRE(sample == 0.5f);
}

TEST_CASE("Each block's input is that same cycle's, at its own position")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    std::atomic<bool> matched{true};
    recording.onProcess = [&](const AudioBlock& block) {
        for (int c = 0; c < block.inputChannels; ++c)
            for (int i = 0; i < block.frames; ++i)
                if (block.inputs[c][i] != counting(c, block.inputPosition + i))
                    matched = false;
    };
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());

    std::minstd_rand random(5);
    for (int i = 0; i < 500; ++i)
        REQUIRE(rig.device.cycle({.frames = 1 + static_cast<std::size_t>(random() % 1500),
                                  .jump = random() % 20 == 0 ? static_cast<std::int64_t>(random() % 2000) : 0}));
    CHECK(matched);
}

TEST_CASE("Positions carry on past 2^31 and 2^32")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());
    REQUIRE(rig.device.cycle({.jump = (std::int64_t{1} << 31) - 600}));
    REQUIRE(rig.device.cycle({.jump = (std::int64_t{1} << 32)}));
    REQUIRE(rig.device.cycle());

    const auto blocks = recording.kept();
    REQUIRE(blocks.size() == 4);
    CHECK(blocks[1].inputPosition == 512 + (std::int64_t{1} << 31) - 600);
    CHECK(blocks[3].inputPosition == 3 * 512 + (std::int64_t{1} << 31) - 600 + (std::int64_t{1} << 32));
    CHECK(blocks[3].inputPosition > std::int64_t{1} << 32);
}

// The timeline

TEST_CASE("A device that stops calling back while it says it runs is found stalled within three blocks")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    for (int i = 0; i < 10; ++i)
        REQUIRE(rig.device.cycle());

    // Two blocks' time with nothing is no stall; four is.
    const auto block = std::chrono::nanoseconds(std::llround(1e9 * 512 / 48000));
    rig.system->advance(2 * block);
    std::this_thread::sleep_for(60ms);
    CHECK(rig.events.count(Kind::stalled) == 0);

    rig.system->advance(2 * block);
    REQUIRE(rig.events.waitFor(Kind::stalled));
    CHECK(recording.frames == 10 * 512);
    CHECK(rig.events.of(Kind::stalled).front().position == 10 * 512);

    REQUIRE(rig.device.cycle());
    CHECK(rig.events.waitFor(Kind::resumed));
}

TEST_CASE("A device calling back far faster than its rate is found running away")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    // Two seconds' blocks in one second of the system's clock.
    for (int i = 0; i < 188; ++i)
        REQUIRE(rig.device.cycle({.advanceNanoseconds = 5'320'000}));
    CHECK(rig.events.waitFor(Kind::runaway));
    CHECK(rig.events.count(Kind::stalled) == 0);
}

TEST_CASE("A block the callbacks take longer over than it lasts is told as late, with how long, its frames and what "
          "the program says made it late, and late blocks within a second of that counted into one event",
          "[timing]")
{
    // A callback that spends as long as it is told in each block, busy as a program working hard is.
    struct Slow : AudioCallback
    {
        std::atomic<std::int64_t> spend{1000};

        void process(const AudioBlock&) noexcept override
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spend.load());
            while (std::chrono::steady_clock::now() < until)
            {
            }
        }
    };

    const auto now = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    Rig rig;
    Slow slow;
    std::mutex askedLock;
    std::vector<std::pair<std::int64_t, std::int64_t>> asked;
    auto stream = rig.stream(stereo(), [&](AudioStream::Options& options) {
        options.describeLate = [&](std::int64_t began, std::int64_t ended) {
            const std::lock_guard hold(askedLock);
            asked.emplace_back(began, ended);
            return std::string("the slow callback");
        };
    });
    REQUIRE(!stream->add(slow));
    REQUIRE(!stream->start());

    // Blocks of 512 frames at 48 kHz last 10.7 ms: ten taking 1 ms aren't late.
    for (int i = 0; i < 10; ++i)
        REQUIRE(rig.device.cycle());
    std::this_thread::sleep_for(60ms);
    CHECK(rig.events.count(Kind::late) == 0);

    // One taking 15 ms is, told at once, the program asked about the time it spanned.
    slow.spend = 15'000;
    const auto before = now();
    REQUIRE(rig.device.cycle());
    const auto after = now();
    slow.spend = 1000;
    REQUIRE(rig.events.waitFor(Kind::late));
    const auto first = rig.events.of(Kind::late).front();
    CHECK(first.count == 1);
    CHECK(first.frames == 512);
    CHECK(first.nanoseconds >= 15'000'000);
    CHECK(first.cause == "the slow callback");
    {
        const std::lock_guard hold(askedLock);
        REQUIRE(asked.size() == 1);
        CHECK(asked.front().first >= before);
        CHECK(asked.front().second <= after);
        CHECK(asked.front().second - asked.front().first >= 15'000'000);
    }

    // Five more at once are told together, a second after the first, the worst of them described.
    for (int i = 0; i < 5; ++i)
    {
        slow.spend = i == 2 ? 30'000 : 15'000;
        REQUIRE(rig.device.cycle());
    }
    slow.spend = 1000;
    for (int i = 0; i < 5; ++i)
        REQUIRE(rig.device.cycle());
    std::this_thread::sleep_for(300ms);
    CHECK(rig.events.count(Kind::late) == 1);
    REQUIRE(rig.events.waitFor(Kind::late, 2, 3000ms));
    const auto second = rig.events.of(Kind::late).back();
    CHECK(second.count == 5);
    CHECK(second.nanoseconds >= 30'000'000);
    CHECK(stream->statistics().overruns == 6);
    std::this_thread::sleep_for(100ms);
    CHECK(rig.events.count(Kind::late) == 2);
}

TEST_CASE("A stream whose first block never comes is stopped, and says why")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());

    rig.system->advance(1s);
    std::this_thread::sleep_for(50ms);
    CHECK(rig.events.count(Kind::noSound) == 0);

    rig.system->advance(1500ms);
    REQUIRE(rig.events.waitFor(Kind::noSound));
    CHECK(soon([&] { return stream->state() == AudioStream::State::stopped; }));
    CHECK(recording.prepares == recording.stops);
    CHECK_FALSE(rig.events.of(Kind::noSound).front().error.message.empty());
}

TEST_CASE("Windows Audio's packets with a discontinuity flag give exactly the gap their positions show")
{
    // Positions 0, 480, 960 and 1920, the flag on the second, which follows on, and the fourth, which jumps.
    Rig rig(fakeDevice("endpoint"));
    rig.device.update([](AudioDeviceInfo& info) {
        info.blockSizes = {480};
        info.currentBlock = 480;
    });
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle({.frames = 480}));
    REQUIRE(rig.device.cycle({.frames = 480, .inputGap = true}));
    REQUIRE(rig.device.cycle({.frames = 480}));
    REQUIRE(rig.device.cycle({.frames = 480, .jump = 480, .inputGap = true}));

    REQUIRE(rig.events.waitFor(Kind::dropout));
    std::this_thread::sleep_for(30ms);
    const auto dropouts = rig.events.of(Kind::dropout);
    std::vector<AudioEvent> input;
    std::copy_if(dropouts.begin(), dropouts.end(), std::back_inserter(input),
                 [](const AudioEvent& e) { return e.direction == AudioDirection::input; });
    REQUIRE(input.size() == 1);
    CHECK(input[0].position == 1440);
    CHECK(input[0].frames == 480);
    CHECK(input[0].reported);
    CHECK(recording.kept().back().inputPosition == 1920);
}

TEST_CASE("A gap flagged with no positions is a dropout of unknown length, and the first cycle's flag isn't")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle({.positions = false, .inputGap = true}));
    REQUIRE(rig.device.cycle({.positions = false}));
    REQUIRE(rig.device.cycle({.positions = false, .inputGap = true}));

    REQUIRE(rig.events.waitFor(Kind::dropout));
    std::this_thread::sleep_for(30ms);
    REQUIRE(rig.events.count(Kind::dropout) == 1);
    CHECK(rig.events.of(Kind::dropout).front().frames == -1);
    CHECK(rig.events.of(Kind::dropout).front().position == 1024);
}

TEST_CASE("A packet the system marks silent reads as zeros, whatever its buffer holds")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    std::atomic<bool> allZero{true};
    recording.onProcess = [&](const AudioBlock& block) {
        for (int c = 0; c < block.inputChannels; ++c)
            for (int i = 0; i < block.frames; ++i)
                if (block.inputs[c][i] != 0.0f)
                    allZero = false;
    };
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle({.inputSilent = true}));
    CHECK(allZero);
}

TEST_CASE("An input giving nothing but exact zeros for a while is said, and noise far down isn't")
{
    Rig rig;
    auto stream = rig.stream(stereo(), [](AudioStream::Options& options) { options.silentInputAfter = 30s; });
    REQUIRE(!stream->start());

    // Noise at -120 dBFS for a minute.
    rig.device.input = [](int, std::int64_t frame) { return frame % 2 == 0 ? 1e-6f : -1e-6f; };
    for (int i = 0; i < 48000 * 60 / 512; ++i)
        REQUIRE(rig.device.cycle());
    std::this_thread::sleep_for(30ms);
    CHECK(rig.events.count(Kind::silentInput) == 0);

    // Then exact zeros for 31 seconds, then sound again.
    rig.device.input = [](int, std::int64_t) { return 0.0f; };
    for (int i = 0; i < 48000 * 31 / 512; ++i)
        REQUIRE(rig.device.cycle());
    REQUIRE(rig.events.waitFor(Kind::silentInput));
    rig.device.input = {};
    REQUIRE(rig.device.cycle());
    CHECK(rig.events.waitFor(Kind::inputSound));
    CHECK(rig.events.count(Kind::silentInput) == 1);
}

TEST_CASE("A rate changed under a running stream, its notices batched from several threads, opens it again once")
{
    Rig rig;
    rig.device.run({.speed = 20.0});
    Recording recording(1 << 16);
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 48000; }));

    rig.device.update([](AudioDeviceInfo& info) { info.currentRate = 44100.0; });
    std::vector<std::thread> senders;
    for (int t = 0; t < 4; ++t)
        senders.emplace_back([&] {
            for (int i = 0; i < 5; ++i)
                rig.device.changed();
        });
    for (auto& sender : senders)
        sender.join();

    REQUIRE(rig.events.waitFor(Kind::formatChanged));
    REQUIRE(soon([&] { return recording.rate == 44100.0 && recording.frames > 200000; }));
    std::this_thread::sleep_for(100ms);
    REQUIRE(!stream->stop());

    CHECK(rig.events.count(Kind::formatChanged) == 1);
    const auto change = rig.events.of(Kind::formatChanged).front();
    CHECK(change.before.sampleRate == 48000.0);
    CHECK(change.after.sampleRate == 44100.0);

    // No block at the new rate before the stream was prepared again, and none at the old one after.
    const auto order = recording.order();
    const auto blocks = recording.kept();
    std::size_t block = 0;
    int prepares = 0;
    for (const auto call : order)
    {
        if (call == Recording::prepared)
            ++prepares;
        else if (call == Recording::processed && block < blocks.size())
            CHECK(blocks[block++].sampleRate == (prepares == 1 ? 48000.0 : 44100.0));
    }
    CHECK(prepares == 2);
    // The rate another program set is kept, never set back.
    CHECK(rig.device.info().currentRate == 44100.0);
}

TEST_CASE("A change that leaves the device as it was opens nothing again")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());
    const auto opens = rig.device.opens();
    rig.device.changed();
    std::this_thread::sleep_for(100ms);
    CHECK(rig.device.opens() == opens);
    CHECK(rig.events.count(Kind::formatChanged) == 0);
}

TEST_CASE("A device asking to be opened again, though nothing about it changed, is opened again once, as it now runs")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());
    const auto opens = rig.device.opens();

    // Asked twice before the change settles, as a driver resetting asks again.
    rig.device.askToReopen();
    rig.device.askToReopen();
    REQUIRE(rig.events.waitFor(Kind::formatChanged));
    std::this_thread::sleep_for(100ms);
    CHECK(rig.device.opens() == opens + 1);
    CHECK(rig.events.count(Kind::formatChanged) == 1);
    CHECK(stream->state() == AudioStream::State::running);

    // A change after that opens nothing again.
    rig.device.changed();
    std::this_thread::sleep_for(100ms);
    CHECK(rig.device.opens() == opens + 1);
}

TEST_CASE("A device's own settings window opens through its running stream, and one without says so")
{
    Rig rig;
    auto stream = rig.stream();
    CHECK(stream->showDeviceSettings().message == "The audio device's settings open while its stream runs.");
    REQUIRE(!stream->start());
    CHECK(stream->showDeviceSettings().message == "The audio device has no settings window of its own.");
    rig.device.hasSettings = true;
    CHECK(!stream->showDeviceSettings());
    CHECK(rig.device.settingsShown.load() == 1);
}

TEST_CASE("A device asking to be opened again as every open starts is opened again once, never for ever")
{
    Rig rig;
    rig.device.reopenInsideStart = true;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.events.waitFor(Kind::formatChanged));
    std::this_thread::sleep_for(300ms);
    CHECK(rig.device.opens() == 2);
    CHECK(rig.events.count(Kind::formatChanged) == 1);
    CHECK(stream->state() == AudioStream::State::running);
}

TEST_CASE("The system muting the program's input is said to a stream with an input as it starts and as it changes, and to no other")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("interface"));
    system->setInputMuted(true);
    Events events;
    AudioStream recording(system, "interface", stereo(true, false), quick(events));
    REQUIRE(!recording.start());
    REQUIRE(events.waitFor(Kind::inputMuted));

    system->add(fakeDevice("speakers", 0, 2));

    system->setInputMuted(false);
    REQUIRE(events.waitFor(Kind::inputUnmuted));
    system->setInputMuted(true);
    REQUIRE(events.waitFor(Kind::inputMuted, 2));
    system->setInputMuted(true);
    std::this_thread::sleep_for(50ms);
    CHECK(events.count(Kind::inputMuted) == 2);
    CHECK(events.count(Kind::inputUnmuted) == 1);
    REQUIRE(!recording.stop());

    // An output alone hears nothing of it.
    Events outputEvents;
    AudioStream output(system, "speakers", stereo(false, true), quick(outputEvents));
    REQUIRE(!output.start());
    system->setInputMuted(false);
    std::this_thread::sleep_for(50ms);
    CHECK(outputEvents.count(Kind::inputMuted) == 0);
    CHECK(outputEvents.count(Kind::inputUnmuted) == 0);
}

TEST_CASE("An output whose callbacks are all idle stops its device after a while, and starts it again when one isn't")
{
    struct Player final : AudioCallback
    {
        std::atomic<bool> quiet{false};
        std::atomic<int> prepares{0}, stops{0};
        void prepare(const AudioStreamInfo&) override { ++prepares; }
        void process(const AudioBlock& block) noexcept override
        {
            for (int c = 0; c < block.outputChannels; ++c)
                std::fill(block.outputs[c], block.outputs[c] + block.frames, 0.0f);
        }
        void stopped() override { ++stops; }
        bool idle() const noexcept override { return quiet.load(); }
    };

    auto system = std::make_shared<FakeAudioSystem>();
    auto& speakers = system->add(fakeDevice("speakers", 0, 2));
    speakers.run({.speed = 1.0});
    Events events;
    auto request = stereo(false, true);
    request.idleAfter = 100ms;
    Player player;
    AudioStream stream(system, "speakers", request, quick(events));
    REQUIRE(!stream.add(player));
    REQUIRE(!stream.start());
    REQUIRE(soon([&] { return speakers.running(); }));

    // Playing, it runs on.
    std::this_thread::sleep_for(250ms);
    CHECK(speakers.running());
    CHECK(events.count(Kind::idle) == 0);

    // Idle a while, the device stops, though the stream is still running to the program and nothing is said lost.
    player.quiet = true;
    REQUIRE(events.waitFor(Kind::idle));
    CHECK_FALSE(speakers.running());
    CHECK(stream.state() == AudioStream::State::running);
    CHECK(player.stops.load() == 1);

    // Wanting sound again, it starts again, prepared again, with no dropout or stall said.
    player.quiet = false;
    REQUIRE(events.waitFor(Kind::active));
    REQUIRE(soon([&] { return speakers.running(); }));
    CHECK(player.prepares.load() == 2);
    std::this_thread::sleep_for(200ms);
    CHECK(events.count(Kind::dropout) == 0);
    CHECK(events.count(Kind::stalled) == 0);
    CHECK(events.count(Kind::noSound) == 0);

    // Stopped by the program while resting, it is stopped.
    player.quiet = true;
    REQUIRE(events.waitFor(Kind::idle, 2));
    REQUIRE(!stream.stop());
    CHECK(stream.state() == AudioStream::State::stopped);
    player.quiet = false;
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(speakers.running());
    CHECK(player.prepares.load() == player.stops.load());
}

TEST_CASE("A stream with an input never stops its device for idle callbacks")
{
    struct Quiet final : AudioCallback
    {
        void process(const AudioBlock&) noexcept override {}
        bool idle() const noexcept override { return true; }
    };
    auto system = std::make_shared<FakeAudioSystem>();
    auto& device = system->add(fakeDevice("interface"));
    device.run({.speed = 1.0});
    Events events;
    auto request = stereo();
    request.idleAfter = 50ms;
    Quiet quiet;
    AudioStream stream(system, "interface", request, quick(events));
    REQUIRE(!stream.add(quiet));
    REQUIRE(!stream.start());
    std::this_thread::sleep_for(300ms);
    CHECK(device.running());
    CHECK(events.count(Kind::idle) == 0);
}

TEST_CASE("An output following the default moves when the default changes and when its device goes, and is lost with none")
{
    auto system = std::make_shared<FakeAudioSystem>();
    auto& speakers = system->add(fakeDevice("speakers", 0, 2));
    auto& headphones = system->add(fakeDevice("headphones", 0, 2));
    auto& airpods = system->add(fakeDevice("airpods", 0, 2));
    system->setDefault(AudioDirection::output, "speakers");
    Events events;
    auto request = stereo(false, true);
    request.followDefault = true;
    Recording recording;
    AudioStream stream(system, "speakers", request, quick(events));
    REQUIRE(!stream.add(recording));
    REQUIRE(!stream.start());
    REQUIRE(speakers.running());

    // The default changes: it moves, prepared again there.
    system->setDefault(AudioDirection::output, "headphones");
    REQUIRE(events.waitFor(Kind::moved));
    CHECK(stream.deviceId() == "headphones");
    CHECK(headphones.running());
    CHECK_FALSE(speakers.running());
    CHECK(events.of(Kind::moved).front().deviceId == "headphones");
    CHECK(recording.prepares == 2);

    // Its device goes, and the default with it: it moves to the new default, never said lost.
    system->setDefault(AudioDirection::output, "airpods", false);
    system->remove("headphones");
    system->setDefault(AudioDirection::output, "airpods");
    REQUIRE(events.waitFor(Kind::moved, 2));
    CHECK(stream.deviceId() == "airpods");
    CHECK(airpods.running());
    CHECK(events.count(Kind::lost) == 0);
    CHECK(stream.state() == AudioStream::State::running);

    // Its device goes with no default to move to: lost.
    system->setDefault(AudioDirection::output, "", false);
    system->remove("airpods");
    REQUIRE(events.waitFor(Kind::lost));
    CHECK(events.of(Kind::lost).front().loss == AudioLoss::unplugged);
}

TEST_CASE("A stream with an input never follows the default, nor one that wasn't asked to")
{
    auto system = std::make_shared<FakeAudioSystem>();
    auto& first = system->add(fakeDevice("first"));
    system->add(fakeDevice("second"));
    system->setDefault(AudioDirection::output, "first");
    Events withInput, notAsked;
    auto duplex = stereo();
    duplex.followDefault = true;
    AudioStream recording(system, "first", duplex, quick(withInput));
    REQUIRE(!recording.start());
    system->setDefault(AudioDirection::output, "second");
    std::this_thread::sleep_for(150ms);
    CHECK(recording.deviceId() == "first");
    CHECK(first.running());
    CHECK(withInput.count(Kind::moved) == 0);
    REQUIRE(!recording.stop());

    AudioStream output(system, "first", stereo(false, true), quick(notAsked));
    REQUIRE(!output.start());
    system->setDefault(AudioDirection::output, "first");
    system->setDefault(AudioDirection::output, "second");
    std::this_thread::sleep_for(150ms);
    CHECK(output.deviceId() == "first");
    CHECK(notAsked.count(Kind::moved) == 0);
}

TEST_CASE("A system that gives no workgroup gives an empty one, which a thread joins to no effect")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    const auto group = stream->workgroup();
    CHECK(group.empty());
    CHECK(group.maxThreads() == 1);
    CHECK_FALSE(group.join(48000.0, 512).joined());
}

TEST_CASE("A device that lost its clock ends its stream, saying so")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());
    rig.device.lose(AudioLoss::clockLost);
    REQUIRE(rig.events.waitFor(Kind::lost));
    const auto lost = rig.events.of(Kind::lost).front();
    CHECK(lost.loss == AudioLoss::clockLost);
    CHECK(lost.error.message == "The audio device lost its clock.");
    CHECK(stream->state() == AudioStream::State::lost);
}

// Losing a device

TEST_CASE("A device unplugged during a take ends it with exactly its frames, and nothing takes its place")
{
    auto system = std::make_shared<FakeAudioSystem>();
    auto& first = system->add(fakeDevice("first"));
    auto& second = system->add(fakeDevice("second"));
    system->setDefault(AudioDirection::input, "second");
    Events events;
    Recording recording;
    AudioStream stream(system, "first", stereo(true, false), quick(events));
    REQUIRE(!stream.add(recording));
    REQUIRE(!stream.start());
    for (int i = 0; i < 25; ++i)
        REQUIRE(first.cycle());

    system->remove("first");
    REQUIRE(events.waitFor(Kind::lost));
    CHECK(recording.frames == 25 * 512);
    CHECK(events.of(Kind::lost).front().deviceId == "first");
    CHECK(events.of(Kind::lost).front().loss == AudioLoss::unplugged);
    CHECK(stream.state() == AudioStream::State::lost);
    CHECK(recording.prepares == recording.stops);
    CHECK(second.opens() == 0);
}

TEST_CASE("A device that goes quiet with no word, then leaves the list, is lost")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());
    rig.system->remove(rig.device.info().id, true);
    REQUIRE(rig.events.waitFor(Kind::lost));
    CHECK(rig.events.of(Kind::lost).front().loss == AudioLoss::unplugged);
}

TEST_CASE("Removing an unrelated device, or adding and removing one a hundred times, changes nothing for a take")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    for (int i = 0; i < 100; ++i)
    {
        rig.system->add(fakeDevice("visitor " + std::to_string(i)));
        REQUIRE(rig.device.cycle());
        rig.system->remove("visitor " + std::to_string(i));
    }
    std::this_thread::sleep_for(50ms);

    CHECK(stream->state() == AudioStream::State::running);
    CHECK(rig.device.opens() == 1);
    CHECK(recording.frames == 100 * 512);
    CHECK(rig.events.count(Kind::lost) == 0);
}

TEST_CASE("The audio service restarting loses the stream, and starting again opens it anew")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());
    rig.system->restartService();
    REQUIRE(rig.events.waitFor(Kind::lost));
    CHECK(rig.events.of(Kind::lost).front().loss == AudioLoss::serviceRestarted);
    CHECK(soon([&] { return stream->state() == AudioStream::State::lost; }));

    CHECK(!stream->start());
    CHECK(rig.device.opens() == 2);
    CHECK(rig.device.cycle());
}

TEST_CASE("A device another program takes for its own is lost with that reason")
{
    Rig rig;
    auto stream = rig.stream();
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());
    rig.device.lose(AudioLoss::takenExclusively, 0x88890004);
    REQUIRE(rig.events.waitFor(Kind::lost));
    const auto lost = rig.events.of(Kind::lost).front();
    CHECK(lost.loss == AudioLoss::takenExclusively);
    CHECK(lost.error.code == 0x88890004);
}

// Sleep

TEST_CASE("Sleep ends the stretch, nothing starts while asleep or just after waking, and the gap is said")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    rig.system->notify(AudioSystemChange::sleeping);
    REQUIRE(rig.events.waitFor(Kind::sleeping));
    CHECK(soon([&] { return stream->state() == AudioStream::State::sleeping; }));
    CHECK(recording.stops == 1);

    rig.system->advance(600s);
    rig.system->notify(AudioSystemChange::woke);
    rig.system->advance(1s);
    std::this_thread::sleep_for(60ms);
    CHECK(rig.device.opens() == 1);

    rig.system->advance(3s);
    REQUIRE(rig.events.waitFor(Kind::woke));
    CHECK(rig.device.opens() == 2);
    CHECK(rig.events.of(Kind::woke).front().nanoseconds >= std::int64_t{600'000'000'000});
    CHECK(soon([&] { return stream->state() == AudioStream::State::running; }));
    CHECK(rig.device.cycle());
    CHECK(recording.prepares == 2);
}

// Choosing a device's settings

TEST_CASE("A device asked for the rate and block size it runs at is never set to them")
{
    Rig rig;
    rig.device.failWhenSetToCurrent = true;
    auto request = stereo();
    request.sampleRate = 48000.0;
    request.blockSize = 512;
    auto stream = rig.stream(request);
    CHECK(!stream->start());
    CHECK(stream->state() == AudioStream::State::running);
    CHECK(rig.device.opens() == 1);
    CHECK(rig.system->sets() == 0);
}

TEST_CASE("A change that fails to open leaves the stream running as it was")
{
    Rig rig;
    Recording recording;
    auto stream = rig.stream();
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(rig.device.cycle());

    auto request = stereo();
    request.sampleRate = 32000.0;
    CHECK(stream->change(request));
    CHECK(stream->state() == AudioStream::State::running);
    CHECK(stream->info().sampleRate == 48000.0);
    CHECK(rig.device.cycle());
    CHECK(recording.prepares == recording.stops + 1);
}
