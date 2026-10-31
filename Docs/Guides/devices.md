# Audio devices

The devices module plays and records through the computer's audio devices: Core Audio on a Mac, and Windows Audio and ASIO on Windows. Devices stop, vanish, change rate and lose samples, and this module tells your program of each as an event, so a recording is never quietly wrong.

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::devices)
```

[`Examples/PlayTone`](../../Examples/PlayTone/PlayTone.cpp) is a whole program in about 120 lines, its listing run as a test on every system. It lists the devices and, given `--play`, plays a quiet tone on the default output and says what happened. Everything below is about what it does, and what a larger program does beyond it.

```cpp
#include <symbolengine/devices.h>

using namespace symbolengine;
```

That header gives you most of the module, and the table at the end names what it leaves out. On Linux the module builds and lists no systems, and its fake system works as it does everywhere. ASIO is built only when you give CMake your own copy of Steinberg's SDK, with `-DSYMBOLENGINE_ASIO_SDK=<the folder holding common/asio.h>`.

## Threads

Four threads matter:

- **Your thread** makes and uses `AudioDevices`. In a program with a message loop, that's the message thread.
- **The audio thread** calls `AudioCallback::process`, which never allocates, frees, locks, waits or calls the system.
- **Each stream's control thread** calls `AudioCallback::prepare` and `stopped`, and makes every call to the system for that stream.
- **The event thread** calls your `onEvent`. You choose it with `post`, and without one it is a thread of the module's own.

A stream's calls return within `AudioStream::Options::callTimeout`, 5 seconds unless you change it, whatever the device does. From its own callbacks they return an error at once, where they would deadlock, so a callback that wants its stream stopped sets a flag for your thread to act on.

## Listing devices

```cpp
AudioDevices devices;

for (const auto& system : devices.systems())
    for (const auto& device : devices.devices(system))
        std::printf("%s: %s, %d in, %d out\n", system.c_str(), device.name.c_str(), device.inputs(), device.outputs());
```

Making an `AudioDevices` reads each device's properties and opens nothing, so it is safe as your program starts and disturbs no other program's sound. No devices and no default are ordinary states, with empty lists and an empty `defaultDevice`. A device that doesn't answer within `describeTimeout` is listed with `answering` false and a `problem`, and the rest are listed.

Keep a device by its `id`, which stays the same when the device comes back. Names repeat and change, and `sameNameIndex` tells two of one interface apart. `AudioDeviceInfo` also holds its channels' names, the rates and block sizes it offers and runs at, its `transport`, its `clockDomain`, and whether it is a Bluetooth headset in its `handsFree` profile or a `loopback` device. An ASIO driver not loaded since your program began is listed by its name alone with `unread` true, as loading drivers to list them hangs programs, and its channels and rates are read when a stream first opens it.

The lists hold what was last read. They are read again when the system says something changed, and the `devicesChanged` and `defaultChanged` events tell you.

## Playing

A stream is one device's input, output or both, and you play and record through it with an `AudioCallback`:

```cpp
class Tone final : public AudioCallback
{
public:
    void prepare(const AudioStreamInfo& info) override { step = 440.0 / info.sampleRate; }

    void process(const AudioBlock& block) noexcept override
    {
        for (auto i = 0; i < block.frames; ++i)
        {
            const auto sample = static_cast<float>(0.03 * std::sin(2.0 * std::numbers::pi * phase));
            phase = std::fmod(phase + step, 1.0);

            for (auto c = 0; c < block.outputChannels; ++c)
                block.outputs[c][i] = sample;
        }
    }

private:
    double step = 0.0;
    double phase = 0.0;
};
```

`prepare` comes before the first block, with the stream's settings as they finally are. A block holds any number of frames from 1 to `info.maxBlock`, so size your buffers there. Outputs arrive silent, and several callbacks in one stream are added together. A stream the system rebuilt, at a new rate say, stops each callback and prepares it again, so `prepare` is where to read the rate.

Open a stream on a device, by its system's name and its `id`. `systems()` is empty on a computer with none, so a real program checks first:

```cpp
const auto system = devices.systems().front();
const auto output = devices.defaultDevice(system, AudioDirection::output);

AudioStreamRequest request;
request.outputChannels = {0, 1};

AudioError error;
auto stream = devices.open(system, output, request, error);
```

Channels are the device's own, numbered from 0, in the order you want them in each block. One you don't take stays silent. A `sampleRate` or `blockSize` of 0 means what the device runs at now, which is right unless the user chose otherwise.

A device takes one stream in your program, and a second `open` is refused until the first stream has gone. Keep the `AudioDevices` for longer than any stream it opened, and each callback for longer than the stream it is in. `open` starts nothing:

```cpp
Tone tone;
stream->add(tone);

if (const auto problem = stream->start())
    showError(problem.message);
```

`stop` returns once each callback has had its last block and been told `stopped`. `info` says what the device really runs at, its latency in frames, -1 when the system doesn't say, and the priority its audio thread got. `change` asks for other channels, rate or block size, opening the device again if it runs, and one that fails to open leaves the stream as it was.

On Windows, `exclusive` in the request asks for Windows Audio's exclusive mode, and a refusal is an error, never shared mode in its place. `showDeviceSettings` opens an ASIO driver's control panel while its stream runs.

## Recording

Take input channels in the request, and read each block's `inputs`. Nothing on the audio thread may touch a disk, so write through the files module's `BackgroundAudioWriter`, which copies samples into a buffer and writes them on a thread of its own:

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::devices SymbolEngine::files)
```

```cpp
class Take final : public AudioCallback
{
public:
    explicit Take(BackgroundAudioWriter& into) : file(into) {}

    void process(const AudioBlock& block) noexcept override
    {
        if (!file.write(block.inputs, static_cast<std::size_t>(block.frames)))
            overflowed = true;
    }

    std::atomic<bool> overflowed{false};

private:
    BackgroundAudioWriter& file;
};
```

Here the writer has room for 240,000 frames, five seconds at 48 kHz. Once the stream has stopped, `file.finish()` completes the file:

```cpp
AudioFileSettings settings;
settings.sampleRate = input.currentRate;

BackgroundAudioWriter file(files::createAudio("take.wav", settings), 240000);
Take take(file);

AudioStreamRequest request;
request.inputChannels = {0, 1};

AudioError error;
auto stream = devices.open(system, input.id, request, error);
stream->add(take);
stream->start();
```

`block.inputPosition` counts every frame since the stretch began, lost ones too, so a block after a gap falls where its sound really fell. A `dropout` event says where the gap is and how long it is, so you can fill it with silence or mark it, and the recording keeps real time. A stretch is one run of the device, and positions start from 0 with each. When a device changes rate under a take, samples at the new rate don't belong in the old file, so start a new one at the `formatChanged` event.

On a Mac the user allows each program to use the microphone once, and Core Audio gives a program it hasn't allowed silence, with no error. The module refuses such an input as it starts, with an error whose `step` is `"microphone permission"`, and asks then if the user hasn't been asked. Ask when the user first chooses to record instead, with `Microphone.h`:

```cpp
if (microphoneAccess() == MicrophoneAccess::notAsked)
    askForMicrophone([](bool allowed) { startRecording(allowed); });
```

The answer comes on a thread of the system's. An app built with `symbolengine_add_app` gives the sentence the system shows as `MICROPHONE_TEXT`, and a program run from a terminal is allowed or not as the terminal is. Elsewhere it reads as allowed, and the system says itself when an input is refused.

## Events

Everything that happens to a device reaches you as an event, delivered in order on the thread you choose:

```cpp
AudioDevices::Options options;
options.post = ui::messageLoop::callAsync;
options.onEvent = [](const AudioEvent& event) {
    if (event.kind == AudioEvent::Kind::dropout)
        markGap(event.position, event.frames);
    else if (event.kind == AudioEvent::Kind::lost)
        showError(event.error.message);
};

AudioDevices devices(options);
```

Every stream it opens delivers there too, and a stream's events name its `system`, `deviceId` and `deviceName`. Other settings for the streams go in `options.streams`.

| Kind | What happened | Also in the event |
|---|---|---|
| `started`, `stopped` | The stream started, or stopped as you asked | `after`, for `started` |
| `lost` | The device stopped while it ran. The stream stays stopped until you start it again | `loss`, `error` |
| `formatChanged` | Its rate, format, channels or block size changed, and it was opened again as it now is, with what it still has of the channels asked for, and those that come back when they do | `before`, `after` |
| `dropout` | Frames were lost | `direction`, `position`, `frames`, -1 when the system didn't say how many |
| `stalled`, `resumed` | It stopped calling back while saying it ran, then started again | `position` |
| `runaway` | It called back far faster than its rate | `position` |
| `late` | Your callbacks took longer over a block than it lasts. Those within a second are told together | `count`, `nanoseconds`, `frames`, `cause` |
| `noSound` | No block came after it started, and the stream stopped | `error` |
| `silentInput`, `inputSound` | An input gave only exact zeros for a while, as a muted microphone or dead cable does, then sound again | `position` |
| `inputMuted`, `inputUnmuted` | The system muted your input, as a headset's own button does on a Mac, or unmuted it | |
| `moved` | A stream following the default moved to the new default | `deviceId` |
| `idle`, `active` | A stream stopped its device while every callback was idle, and started it again | |
| `sleeping`, `woke` | The computer slept and woke. The stream starts again by itself | `nanoseconds`, for `woke` |
| `devicesChanged` | A device came or went | `system` |
| `defaultChanged` | The default for a direction changed | `system`, `deviceId`, `direction` |

Nothing takes a lost device's place. Its stream stays stopped, and `start` opens it again once it comes back, by its `id`.

`stream->statistics()` gives the counts from any thread: `dropouts`, `lostFrames`, `overruns` and the smoothed `load`. To have a `late` event name what made a block late, give `options.streams.describeLate`, as the [hosting guide](host.md#timing) shows.

## Keeping the user's choice

An `AudioSetup` holds the devices a user chose, each direction apart: a device, the system's default, or none. Its input starts as none and its output as the default. Save it as a `Value`, here in a `SettingsFile`:

```cpp
AudioSetup setup;
setup.system = system;
setup.choose(AudioDirection::output, device);
settings.set("audio", setup.toValue());
```

When your program starts again, resolve it against the devices there are now, and open what it found. A setup read from nothing names no system, and `resolve` then looks on the first while `open` needs a name, so give it one:

```cpp
auto setup = AudioSetup::fromValue(settings.get("audio"));

if (setup.system.empty())
    setup.system = devices.systems().front();

const auto found = devices.resolve(setup).output;

if (!found.said.empty())
    tellUser(found.said);

if (!found.id.empty())
{
    AudioStreamRequest request;
    request.outputChannels = found.channels;
    request.sampleRate = found.sampleRate;
    request.blockSize = found.blockSize;
    request.exclusive = setup.exclusive;
    stream = devices.open(setup.system, found.id, request, error);
}
```

The input resolves the same way, and when both directions find one device, take both sets of channels in one stream.

Each device is found by its `id`, then by a name only one device has. One that's gone is put in place direction by direction, so a missing input leaves the output as it was, and never by a headset's hands-free microphone or a loopback device the user hasn't chosen before. A rate no longer offered becomes the nearest, and a block size the next larger. Every substitution is a sentence for the user in `said`, and `exact`, `byName` and `channelsMissing` say the same for your code. When several devices share the saved name, `ambiguous` lists them for the user to choose between, and `id` stays empty.

Resolve again at `devicesChanged`, to take back a device that returned while nothing records. Save what the user chose, never what the device happened to give.

## Two devices, two clocks

An input on one device and an output on another run on two clocks that drift apart, unless their `clockDomain`s are the same and not 0. To hear an input through another device's output, add an `AudioBridge` to both streams. It holds the delay where you set it for as long as they run, by resampling what crosses:

```cpp
AudioBridge bridge({.channels = 2, .delay = 30ms});

input->add(bridge.input());
output->add(bridge.output());
input->start();
output->start();
```

The delay is held to at least what an input block and an output block need at once, whatever you set. `bridge.state()` gives the measured `drift`, the `delaySeconds` now, whether it has `settled`, and any `underruns` and `overruns`. Write a recording from the input's own stream, never through the bridge, so its samples stay the interface's own.

## Following the default, and resting when idle

```cpp
AudioStreamRequest request;
request.outputChannels = {0, 1};
request.followDefault = true;
request.idleAfter = 2s;
```

`followDefault` moves an output-only stream, opened on the system's default, whenever the default changes or its device goes, as AirPods connecting move a Mac's sound. The `moved` event tells you, and your callback is prepared again there. A stream with an input never moves.

`idleAfter` stops the device once every callback's `idle` has said true for that long, and starts it again when one says false, with `idle` and `active` events, and `stopped` and `prepare` for your callback. An output running in silence keeps a Mac's AirPods from moving to another device and keeps any computer from saving power. `idle` is read while blocks run, so answer it from an atomic. It never applies to a stream with an input.

## Playing files

`AudioPlayer` reads ahead on a thread of its own and hands the audio thread what it read, so a slow disk never holds up a block. Give it a source, whose `read` may take as long as it needs. Where it can't read, as for a file on a drive just unplugged, it throws: what it was to give plays as silence, and `AudioPlayer::Event::Kind::readFailed` says so once each play:

```cpp
class FileSource final : public AudioPlayerSource
{
public:
    explicit FileSource(const std::filesystem::path& path) : file(files::openAudio(path)) {}

    int channels() const override { return file->info().channels; }
    double sampleRate() const override { return file->info().sampleRate; }
    std::int64_t length() const override { return static_cast<std::int64_t>(file->info().frames); }

    void read(float* const* into, std::int64_t at, std::size_t frames) override
    {
        std::error_code error;
        const auto done = file->read(static_cast<std::uint64_t>(at), into, frames, error);

        for (auto c = 0; c < channels(); ++c)
            std::fill(into[c] + done, into[c] + frames, 0.0f);
    }

private:
    std::unique_ptr<AudioFileReader> file;
};
```

Then a callback renders it, and `idle` lets the device rest while nothing plays:

```cpp
class Deck final : public AudioCallback
{
public:
    void prepare(const AudioStreamInfo& info) override { player.prepare(info.sampleRate, info.outputChannels); }

    void process(const AudioBlock& block) noexcept override
    {
        player.render(block.outputs, block.outputChannels, block.frames);
    }

    bool idle() const noexcept override { return !player.playing(); }

    AudioPlayer player;
};
```

Once the stream runs, play from any thread but the audio thread:

```cpp
const auto source = std::make_shared<FileSource>("take.wav");

deck.player.play(source, 0);          // once, from the start
deck.player.play(source, 0, -1, 0);   // from the start, looping the whole file
```

`seek` jumps, `stop` fades out and returns at once, and `playing` and `position` say where it is. A source at another rate plays at the device's, and a mono source plays on every output. When the reader falls behind, playback keeps time, going silent for what it lacks. `AudioPlayer::Options::onEvent` says where with `underrun`, and says `finished` when the last frame has been heard.

## Threads of your own

A thread doing work the audio thread waits for joins the stream's workgroup, so the system schedules it to the device's deadline. On Apple Silicon, work spread over threads needs this to finish in time:

```cpp
const auto group = stream->workgroup();
const auto info = stream->info();

std::thread worker([group, info] {
    const auto membership = group.join(info.sampleRate, info.maxBlock);
    doWork();
});
```

It lasts as long as `membership` does, and joining an empty workgroup does nothing. Take the workgroup again after a stream is opened again, and `maxThreads` suggests how many threads to use.

## Testing without hardware

`FakeAudioSystem` takes the place of the computer's system, so everything above runs in your tests with no audio hardware. Its devices are what you add, its name is `Fake` unless you give another, and each runs at 48 kHz in blocks of 512 unless you say otherwise. A device has as many channels as it has names:

```cpp
auto system = std::make_shared<FakeAudioSystem>();

AudioDeviceInfo speakers;
speakers.id = "speakers";
speakers.name = "Speakers";
speakers.outputNames = {"Left", "Right"};

auto& device = system->add(speakers);
system->setDefault(AudioDirection::output, "speakers");

AudioDevices::Options options;
options.systems = {system};
AudioDevices devices(options);
```

From there it is the same program, except that each block comes when you say. `cycle` gives one on the calling thread, as the audio thread would, and moves the system's clock on:

```cpp
AudioStreamRequest request;
request.outputChannels = {0, 1};

AudioError error;
auto stream = devices.open("Fake", "speakers", request, error);

Tone tone;
stream->add(tone);
stream->start();

device.cycle();
const auto heard = device.lastOutput();
```

`heard` holds a vector of samples for each channel the stream took. Then make it misbehave, each line a thing real hardware does:

```cpp
device.cycle({.jump = 480});                                                 // 480 frames lost before this block
device.changed([](AudioDeviceInfo& info) { info.currentRate = 44100.0; });   // its rate changed under the stream
device.lose(AudioLoss::takenExclusively);                                    // another program took it
system->remove("speakers");                                                  // unplugged
system->notify(AudioSystemChange::sleeping);                                 // the computer is going to sleep
system->restartService();                                                    // every stream on it is lost
system->advance(3s);                                                         // time passes with no blocks
```

The module's watchdogs read the system's clock, which moves only as its devices call back or you `advance` it, so a test decides exactly when a stall or a missing first block is found. Set `failOpen`, `failStart` or `failDescribe` on a device and that call fails. `hangDescribe` and `hangStop` make those calls never return, `callAfterStop` gives one more block after a stop, and `cycleInsideStart` and `reopenInsideStart` do what some drivers do as they start. An input channel gives a counting sequence unless you set `device.input`, so a test can see exactly which frame arrived.

For a device that runs by itself, ask before the stream starts. This one runs in real time on a thread of its own, its clock 100 parts per million fast:

```cpp
device.run({.speed = 1.0, .ppm = 100.0});
```

`system->opens()`, `system->sets()`, `device.starts()` and `device.lastRequest()` say what was asked of it, so a test can show that listing opened nothing. Events arrive on another thread, so a test waits for them with a limit. [`Modules/devices/Tests/Helpers.h`](../../Modules/devices/Tests/Helpers.h) has small helpers for that to copy, which also shorten `settle` and `watchEvery` so changes are noticed quickly, and the tests beside it show each of these in use.

To check your callbacks allocate nothing, include `AudioThreadChecks.h` in one source file of your tests. It replaces `operator new` and `delete`, and counts every allocation and free made on the audio thread:

```cpp
const auto before = audioThread::allocations();
device.cycle();
const auto allocated = audioThread::allocations() - before;
```

`RecordingAudioSystem` wraps a system and writes what happens to it as lines of JSON, and `replay` plays such lines back at the speed given, so a test meets what real hardware did:

```cpp
system->replay(lines, 1000.0);
```

The computer's own systems are made inside `AudioDevices` and aren't in the public headers, so to record real hardware use `symbolengine_devices_recorder`, built with the module's tests, as `symbolengine_devices_recorder <seconds> <file.jsonl> [part of the input's name] [system]`. It records from an input while it runs, and `Modules/devices/Tests/Sessions` holds sessions made with it.

## What's in the reference

| Header | What it gives you |
|---|---|
| `AudioDevices.h` | `AudioDevices`: the lists, the defaults and the streams opened on them |
| `AudioDevice.h` | `AudioDeviceInfo`, `AudioStreamRequest`, `AudioStreamInfo`, `AudioError` |
| `AudioStream.h` | `AudioStream`, `AudioCallback`, `AudioBlock`, `AudioEvent` |
| `AudioSetup.h` | `AudioSetup`, `AudioResolution` |
| `AudioPlayer.h` | `AudioPlayer`, `AudioPlayerSource` |
| `AudioSystem.h`, `AudioWorkgroup.h` | The narrow layer each system is written against, and a stream's workgroup |
| `AudioThread.h` | Whether a thread is inside a device's cycle, and the count of allocations made there |
| `FakeAudioSystem.h` | A scripted system for your own tests |
| `AudioBridge.h` | An input carried to an output on another clock; include it by name |
| `Microphone.h` | Whether your program may record, and asking; include it by name |
| `RecordingAudioSystem.h` | What a system did, written down for the fake to replay; include it by name |
| `AudioThreadChecks.h` | Allocations on the audio thread counted, for one source file of your tests; include it by name |

The [architecture's devices section](../02_architecture.md#devices) holds every problem audio software has met with these systems and what this module does about each.
