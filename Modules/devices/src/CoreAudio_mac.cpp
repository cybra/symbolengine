#include "AudioWorkgroupDetail.h"
#include "CoreAudioDetail.h"
#include "Systems.h"

#include <symbolengine/devices/AudioSystem.h>
#include <symbolengine/devices/Microphone.h>

#include <Block.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// Core Audio behind the devices module's system layer. Every property read can fail and is checked for the size it
// gives. Listeners are blocks on a private queue, so notifications come whatever the main thread is doing, and they
// only pass on what they say; overloads come on the audio thread and only set a flag. A stream takes the device's
// buffers as Core Audio gives them, a list of streams each interleaving its own channels, and hands the devices
// module one buffer per channel it asked for; every output buffer is cleared before anything else in a cycle.
namespace symbolengine
{

namespace
{
namespace detail = coreAudio::detail;

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                   AudioObjectPropertyElement element = kAudioObjectPropertyElementMain)
{
    return {selector, scope, element};
}

template <typename T>
bool read(AudioObjectID object, const AudioObjectPropertyAddress& where, T& value)
{
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &value) == noErr && size == sizeof(T);
}

std::vector<std::uint8_t> readBytes(AudioObjectID object, const AudioObjectPropertyAddress& where)
{
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &where, 0, nullptr, &size) != noErr || size == 0)
        return {};
    std::vector<std::uint8_t> bytes(size);
    if (AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, bytes.data()) != noErr)
        return {};
    bytes.resize(size);
    return bytes;
}

std::string utf8(CFStringRef text)
{
    if (text == nullptr)
        return {};
    if (const char* fast = CFStringGetCStringPtr(text, kCFStringEncodingUTF8))
        return fast;
    const auto length = CFStringGetLength(text);
    std::vector<char> bytes(static_cast<std::size_t>(CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1));
    if (!CFStringGetCString(text, bytes.data(), static_cast<CFIndex>(bytes.size()), kCFStringEncodingUTF8))
        return {};
    return bytes.data();
}

std::string readText(AudioObjectID object, const AudioObjectPropertyAddress& where)
{
    CFStringRef text = nullptr;
    if (!read(object, where, text) || text == nullptr)
        return {};
    auto result = utf8(text);
    CFRelease(text);
    return result;
}

std::string uidOf(AudioObjectID device)
{
    return readText(device, address(kAudioDevicePropertyDeviceUID));
}

AudioObjectID deviceFor(const std::string& uid)
{
    const auto text = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    if (text == nullptr)
        return kAudioObjectUnknown;
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    const auto where = address(kAudioHardwarePropertyTranslateUIDToDevice);
    const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, sizeof(CFStringRef), &text,
                                                   &size, &device);
    CFRelease(text);
    return status == noErr ? device : kAudioObjectUnknown;
}

// Each of the device's streams' channel counts that way, in order.
std::vector<int> streamChannels(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const auto bytes = readBytes(device, address(kAudioDevicePropertyStreamConfiguration, scope));
    std::vector<int> channels;
    if (bytes.size() < offsetof(AudioBufferList, mBuffers))
        return channels;
    const auto* list = reinterpret_cast<const AudioBufferList*>(bytes.data());
    const auto room = (bytes.size() - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer);
    for (UInt32 b = 0; b < list->mNumberBuffers && b < room; ++b)
        channels.push_back(static_cast<int>(list->mBuffers[b].mNumberChannels));
    return channels;
}

int total(const std::vector<int>& channels)
{
    int sum = 0;
    for (const auto count : channels)
        sum += count;
    return sum;
}

AudioTransport transportOf(UInt32 type)
{
    switch (type)
    {
        case kAudioDeviceTransportTypeBuiltIn: return AudioTransport::builtIn;
        case kAudioDeviceTransportTypeUSB: return AudioTransport::usb;
        case kAudioDeviceTransportTypeFireWire: return AudioTransport::firewire;
        case kAudioDeviceTransportTypeThunderbolt: return AudioTransport::thunderbolt;
        case kAudioDeviceTransportTypePCI: return AudioTransport::pci;
        case kAudioDeviceTransportTypeBluetooth:
        case kAudioDeviceTransportTypeBluetoothLE: return AudioTransport::bluetooth;
        case kAudioDeviceTransportTypeHDMI: return AudioTransport::hdmi;
        case kAudioDeviceTransportTypeDisplayPort: return AudioTransport::displayPort;
        case kAudioDeviceTransportTypeAirPlay: return AudioTransport::airPlay;
        case kAudioDeviceTransportTypeAVB: return AudioTransport::network;
        case kAudioDeviceTransportTypeVirtual: return AudioTransport::virtualDevice;
        case kAudioDeviceTransportTypeAggregate:
        case kAudioDeviceTransportTypeAutoAggregate: return AudioTransport::aggregate;
        default: return AudioTransport::unknown;
    }
}

std::vector<std::string> channelNames(AudioObjectID device, AudioObjectPropertyScope scope, int count,
                                      const char* fallback)
{
    std::vector<std::string> names;
    for (int c = 0; c < count; ++c)
    {
        auto name = readText(device, address(kAudioObjectPropertyElementName, scope, static_cast<UInt32>(c + 1)));
        names.push_back(name.empty() ? std::string(fallback) + " " + std::to_string(c + 1) : std::move(name));
    }
    return names;
}

std::uint32_t timebaseNumerator = 1, timebaseDenominator = 1;
std::once_flag timebaseRead;

std::int64_t nanosecondsFromHost(std::uint64_t ticks)
{
    std::call_once(timebaseRead, [] {
        mach_timebase_info_data_t timebase{};
        mach_timebase_info(&timebase);
        timebaseNumerator = timebase.numer;
        timebaseDenominator = timebase.denom;
    });
    return detail::nanosecondsFromTicks(ticks, timebaseNumerator, timebaseDenominator);
}

// Sets a property, waits a bounded time for Core Audio to say it changed, and says whether it reads back as set.
template <typename T>
bool setAndWait(AudioObjectID device, const AudioObjectPropertyAddress& where, T value, bool same(T, T))
{
    T now{};
    if (read(device, where, now) && same(now, value))
        return true;

    const auto changed = dispatch_semaphore_create(0);
    const auto queue = dispatch_queue_create("symbolengine.coreaudio.set", DISPATCH_QUEUE_SERIAL);
    AudioObjectPropertyListenerBlock heard =
        Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { dispatch_semaphore_signal(changed); });
    AudioObjectAddPropertyListenerBlock(device, &where, queue, heard);

    bool set = AudioObjectSetPropertyData(device, &where, 0, nullptr, sizeof(T), &value) == noErr;
    if (set)
        dispatch_semaphore_wait(changed, dispatch_time(DISPATCH_TIME_NOW, 2 * static_cast<std::int64_t>(NSEC_PER_SEC)));

    AudioObjectRemovePropertyListenerBlock(device, &where, queue, heard);

    // One already on its way would signal what is about to be let go of.
    dispatch_sync(queue, ^{
                  });
    Block_release(heard);
    dispatch_release(queue);
    dispatch_release(changed);
    return set && read(device, where, now) && same(now, value);
}

bool sameRate(Float64 a, Float64 b)
{
    return std::abs(a - b) < 0.5;
}

bool sameFrames(UInt32 a, UInt32 b)
{
    return a == b;
}

std::int64_t latencyOf(AudioObjectID device, AudioObjectPropertyScope scope, UInt32 block)
{
    UInt32 latency = 0, safety = 0, streamLatency = 0;
    read(device, address(kAudioDevicePropertyLatency, scope), latency);
    read(device, address(kAudioDevicePropertySafetyOffset, scope), safety);
    const auto streams = readBytes(device, address(kAudioDevicePropertyStreams, scope));
    if (streams.size() >= sizeof(AudioStreamID))
    {
        AudioStreamID first = 0;
        std::memcpy(&first, streams.data(), sizeof(first));
        read(first, address(kAudioStreamPropertyLatency), streamLatency);
    }
    return static_cast<std::int64_t>(latency) + safety + block + streamLatency;
}

// What the listeners of one stream reach, alive for as long as any block that might still run holds it.
struct StreamListeners
{
    std::atomic<AudioSystemClient*> client{nullptr};
    std::atomic<bool> overload{false};
    AudioObjectID device = kAudioObjectUnknown;
};

class CoreAudioStream final : public AudioSystemStream
{
public:
    CoreAudioStream(AudioObjectID d, AudioStreamRequest r, bool askFirst)
        : device(d), request(std::move(r)), ask(askFirst)
    {
    }

    ~CoreAudioStream() override
    {
        stop();
        if (proc != nullptr)
            AudioDeviceDestroyIOProcID(device, proc);
        if (queue != nullptr)
            dispatch_release(queue);
    }

    // Lays out the buffers and the IO proc; false with the error set when it can't.
    bool prepare(AudioError& error)
    {
        inputStreams = streamChannels(device, kAudioObjectPropertyScopeInput);
        outputStreams = streamChannels(device, kAudioObjectPropertyScopeOutput);
        inputPlaces = detail::placesOf(request.inputChannels, inputStreams);
        outputPlaces = detail::placesOf(request.outputChannels, outputStreams);
        for (const auto& place : inputPlaces)
            if (place.buffer < 0)
            {
                error = {"The audio device doesn't have that input channel.", "open"};
                return false;
            }
        for (const auto& place : outputPlaces)
            if (place.buffer < 0)
            {
                error = {"The audio device doesn't have that output channel.", "open"};
                return false;
            }

        // Room for the largest cycle the device can give.
        AudioValueRange range{};
        UInt32 current = 512, variable = 0;
        read(device, address(kAudioDevicePropertyBufferFrameSize), current);
        read(device, address(kAudioDevicePropertyBufferFrameSizeRange), range);
        read(device, address(kAudioDevicePropertyUsesVariableBufferFrameSizes), variable);
        capacity = std::max<std::size_t>({8192, 4 * static_cast<std::size_t>(current),
                                          static_cast<std::size_t>(range.mMaximum), static_cast<std::size_t>(variable)});
        inputs.assign(request.inputChannels.size(), std::vector<float>(capacity, 0.0f));
        outputs.assign(request.outputChannels.size(), std::vector<float>(capacity, 0.0f));
        for (auto& channel : inputs)
            inputPointers.push_back(channel.data());
        for (auto& channel : outputs)
            outputPointers.push_back(channel.data());

        if (AudioDeviceCreateIOProcID(device, &CoreAudioStream::cycle, this, &proc) != noErr || proc == nullptr)
        {
            error = {"The audio device didn't take a stream.", "create IO proc"};
            return false;
        }

        // Streams the stream takes nothing from are switched off, so a program only playing leaves the microphone
        // indicator off.
        useStreams(kAudioObjectPropertyScopeInput, inputPlaces, inputStreams.size());
        useStreams(kAudioObjectPropertyScopeOutput, outputPlaces, outputStreams.size());

        queue = dispatch_queue_create("symbolengine.coreaudio.device", DISPATCH_QUEUE_SERIAL);
        listeners = std::make_shared<StreamListeners>();
        listeners->device = device;
        return true;
    }

    AudioStreamInfo info() const override
    {
        AudioStreamInfo now;
        Float64 rate = 0.0;
        UInt32 block = 0, variable = 0;
        read(device, address(kAudioDevicePropertyNominalSampleRate), rate);
        read(device, address(kAudioDevicePropertyBufferFrameSize), block);
        read(device, address(kAudioDevicePropertyUsesVariableBufferFrameSizes), variable);
        now.sampleRate = rate;
        now.maxBlock = static_cast<int>(std::max(block, variable));
        now.inputChannels = static_cast<int>(request.inputChannels.size());
        now.outputChannels = static_cast<int>(request.outputChannels.size());
        now.inputLatency = request.inputChannels.empty() ? -1 : latencyOf(device, kAudioObjectPropertyScopeInput, block);
        now.outputLatency =
            request.outputChannels.empty() ? -1 : latencyOf(device, kAudioObjectPropertyScopeOutput, block);
        now.priority = "Core Audio's IO thread";
        now.unprocessed = !request.inputChannels.empty();
        return now;
    }

    // The device's audio workgroup, given retained, from macOS 11.
    AudioWorkgroup workgroup() const override
    {
        void* group = nullptr;
        UInt32 size = sizeof(group);
        const auto where = address(kAudioDevicePropertyIOThreadOSWorkgroup);
        if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, &group) != noErr)
            return {};
        return devices::detail::appleWorkgroup(group);
    }

    bool start(AudioSystemClient& to, AudioError& error) override
    {
        listeners->client = &to;
        listen();
        client.store(&to);
        const auto status = AudioDeviceStart(device, proc);
        if (status != noErr)
        {
            client.store(nullptr);
            unlisten();
            error = {"The audio device didn't start.", "AudioDeviceStart", status};
            return false;
        }
        running = true;

        // The user's answer comes later; a no ends the stream, as the system refusing it.
        if (ask)
        {
            ask = false;
            askForMicrophone([weak = std::weak_ptr<StreamListeners>(listeners)](bool allowed) {
                if (const auto held = weak.lock(); held != nullptr && !allowed)
                    if (auto* to = held->client.load())
                        to->lost(AudioLoss::refused, 0);
            });
        }
        return true;
    }

    void stop() noexcept override
    {
        if (!running)
            return;
        running = false;
        AudioDeviceStop(device, proc);
        client.store(nullptr);
        unlisten();
    }

private:
    static OSStatus cycle(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                          const AudioTimeStamp* inputTime, AudioBufferList* output, const AudioTimeStamp* outputTime,
                          void* context)
    {
        auto& self = *static_cast<CoreAudioStream*>(context);

        // Every output buffer cleared first, those the stream doesn't feed among them.
        if (output != nullptr)
            for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
                if (output->mBuffers[b].mData != nullptr)
                    std::memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);

        auto* to = self.client.load();
        if (to == nullptr)
            return noErr;

        // The cycle's length from the buffers' own sizes.
        std::size_t frames = 0;
        const auto lengthOf = [](const AudioBuffer& buffer) -> std::size_t {
            return buffer.mNumberChannels == 0 ? 0 : buffer.mDataByteSize / (buffer.mNumberChannels * sizeof(float));
        };
        if (input != nullptr)
            for (UInt32 b = 0; b < input->mNumberBuffers && frames == 0; ++b)
                if (input->mBuffers[b].mData != nullptr)
                    frames = lengthOf(input->mBuffers[b]);
        if (output != nullptr)
            for (UInt32 b = 0; b < output->mNumberBuffers && frames == 0; ++b)
                if (output->mBuffers[b].mData != nullptr)
                    frames = lengthOf(output->mBuffers[b]);
        frames = std::min(frames, self.capacity);

        for (std::size_t c = 0; c < self.inputPlaces.size(); ++c)
        {
            const auto& place = self.inputPlaces[c];
            const auto b = static_cast<UInt32>(place.buffer);
            if (input == nullptr || b >= input->mNumberBuffers || input->mBuffers[b].mData == nullptr)
            {
                self.inputPointers[c] = nullptr;
                continue;
            }
            auto* to_ = self.inputs[c].data();
            const bool read = detail::readChannel(
                static_cast<const float*>(input->mBuffers[b].mData), input->mBuffers[b].mDataByteSize,
                static_cast<int>(input->mBuffers[b].mNumberChannels), place.channel, frames, to_);
            self.inputPointers[c] = read ? to_ : nullptr;
        }

        AudioSystemCycle given;
        given.inputs = self.inputPointers.data();
        given.outputs = self.outputPointers.data();
        given.inputChannels = static_cast<int>(self.inputPlaces.size());
        given.outputChannels = static_cast<int>(self.outputPlaces.size());
        given.frames = frames;
        const auto valid = [](const AudioTimeStamp* time, UInt32 flag) {
            return time != nullptr && (time->mFlags & flag) != 0;
        };
        given.inputPosition = !self.inputPlaces.empty() && valid(inputTime, kAudioTimeStampSampleTimeValid)
                                  ? static_cast<std::int64_t>(std::llround(inputTime->mSampleTime))
                                  : -1;
        given.outputPosition = !self.outputPlaces.empty() && valid(outputTime, kAudioTimeStampSampleTimeValid)
                                   ? static_cast<std::int64_t>(std::llround(outputTime->mSampleTime))
                                   : -1;
        given.inputTime = valid(inputTime, kAudioTimeStampHostTimeValid) ? nanosecondsFromHost(inputTime->mHostTime) : -1;
        given.outputTime =
            valid(outputTime, kAudioTimeStampHostTimeValid) ? nanosecondsFromHost(outputTime->mHostTime) : -1;
        const bool overloaded = self.listeners->overload.exchange(false);
        given.inputGap = overloaded;
        given.outputGap = overloaded;
        to->cycle(given);

        if (output != nullptr)
            for (std::size_t c = 0; c < self.outputPlaces.size(); ++c)
            {
                const auto& place = self.outputPlaces[c];
                const auto b = static_cast<UInt32>(place.buffer);
                if (b >= output->mNumberBuffers)
                    continue;
                detail::writeChannel(self.outputs[c].data(), frames, static_cast<float*>(output->mBuffers[b].mData),
                                     output->mBuffers[b].mDataByteSize,
                                     static_cast<int>(output->mBuffers[b].mNumberChannels), place.channel);
            }
        return noErr;
    }

    void useStreams(AudioObjectPropertyScope scope, const std::vector<detail::Place>& places, std::size_t streams)
    {
        if (streams == 0)
            return;
        const auto where = address(kAudioDevicePropertyIOProcStreamUsage, scope);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr ||
            size < offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn))
            return;
        std::vector<std::uint8_t> bytes(size);
        auto* usage = reinterpret_cast<AudioHardwareIOProcStreamUsage*>(bytes.data());
        usage->mIOProc = reinterpret_cast<void*>(proc);
        if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, usage) != noErr)
            return;
        const auto count = std::min<std::size_t>(usage->mNumberStreams, streams);
        for (std::size_t s = 0; s < count; ++s)
            usage->mStreamIsOn[s] = std::any_of(places.begin(), places.end(),
                                                [&](const auto& place) { return place.buffer == static_cast<int>(s); });
        AudioObjectSetPropertyData(device, &where, 0, nullptr, size, usage);
    }

    void listen()
    {
        const auto context = listeners;
        changedBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) {
            if (auto* to = context->client.load())
                to->changed();
        });
        aliveBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) {
            UInt32 alive = 1;
            read(context->device, address(kAudioDevicePropertyDeviceIsAlive), alive);
            if (alive == 0)
                if (auto* to = context->client.load())
                    to->lost(AudioLoss::unplugged, 0);
        });
        stoppedBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) {
            if (auto* to = context->client.load())
                to->lost(AudioLoss::notAnswering, 0);
        });
        overloadBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { context->overload = true; });

        for (const auto& where : changes())
            AudioObjectAddPropertyListenerBlock(device, &where, queue, changedBlock);
        const auto alive = address(kAudioDevicePropertyDeviceIsAlive);
        AudioObjectAddPropertyListenerBlock(device, &alive, queue, aliveBlock);
        const auto stopped = address(kAudioDevicePropertyIOStoppedAbnormally);
        AudioObjectAddPropertyListenerBlock(device, &stopped, queue, stoppedBlock);
        const auto overload = address(kAudioDeviceProcessorOverload);
        AudioObjectAddPropertyListenerBlock(device, &overload, queue, overloadBlock);
    }

    void unlisten()
    {
        if (changedBlock == nullptr)
            return;
        for (const auto& where : changes())
            AudioObjectRemovePropertyListenerBlock(device, &where, queue, changedBlock);
        const auto alive = address(kAudioDevicePropertyDeviceIsAlive);
        AudioObjectRemovePropertyListenerBlock(device, &alive, queue, aliveBlock);
        const auto stopped = address(kAudioDevicePropertyIOStoppedAbnormally);
        AudioObjectRemovePropertyListenerBlock(device, &stopped, queue, stoppedBlock);
        const auto overload = address(kAudioDeviceProcessorOverload);
        AudioObjectRemovePropertyListenerBlock(device, &overload, queue, overloadBlock);

        // Those already on their way run before the client goes.
        dispatch_sync(queue, ^{
                      });
        listeners->client = nullptr;
        for (auto* block : {&changedBlock, &aliveBlock, &stoppedBlock, &overloadBlock})
        {
            Block_release(*block);
            *block = nullptr;
        }
    }

    static std::vector<AudioObjectPropertyAddress> changes()
    {
        return {address(kAudioDevicePropertyNominalSampleRate),
                address(kAudioDevicePropertyBufferFrameSize),
                address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput),
                address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput),
                address(kAudioDevicePropertyDeviceHasChanged),
                address(kAudioDevicePropertyClockSource, kAudioObjectPropertyScopeGlobal)};
    }

    AudioObjectID device;
    AudioStreamRequest request;
    AudioDeviceIOProcID proc = nullptr;
    dispatch_queue_t queue = nullptr;
    std::shared_ptr<StreamListeners> listeners;
    AudioObjectPropertyListenerBlock changedBlock = nullptr, aliveBlock = nullptr, stoppedBlock = nullptr,
                                     overloadBlock = nullptr;
    std::atomic<AudioSystemClient*> client{nullptr};
    bool running = false;
    bool ask = false;

    std::vector<int> inputStreams, outputStreams;
    std::vector<detail::Place> inputPlaces, outputPlaces;
    std::size_t capacity = 0;
    std::vector<std::vector<float>> inputs, outputs;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
};

// The system's own notices, and the computer's sleep, passed to each listener; removing one waits for calls under way.
struct SystemListeners
{
    std::mutex lock;
    std::condition_variable idle;
    std::vector<AudioSystemListener*> listeners;
    int callsUnderWay = 0;

    void tell(AudioSystemChange change)
    {
        std::vector<AudioSystemListener*> now;
        {
            const std::lock_guard hold(lock);
            now = listeners;
            ++callsUnderWay;
        }
        for (auto* listener : now)
            listener->systemChanged(change);
        {
            const std::lock_guard hold(lock);
            --callsUnderWay;
        }
        idle.notify_all();
    }
};

class CoreAudioSystem final : public AudioSystem
{
public:
    CoreAudioSystem() : queue(dispatch_queue_create("symbolengine.coreaudio.system", DISPATCH_QUEUE_SERIAL)) {}

    ~CoreAudioSystem() override
    {
        stopListening();
        dispatch_release(queue);
    }

    std::string name() const override { return "Core Audio"; }

    std::int64_t now() const noexcept override { return nanosecondsFromHost(mach_absolute_time()); }

    std::vector<std::string> deviceIds() override
    {
        std::vector<std::string> ids;
        const auto bytes = readBytes(kAudioObjectSystemObject, address(kAudioHardwarePropertyDevices));
        for (std::size_t at = 0; at + sizeof(AudioObjectID) <= bytes.size(); at += sizeof(AudioObjectID))
        {
            AudioObjectID device = 0;
            std::memcpy(&device, bytes.data() + at, sizeof(device));
            if (auto uid = uidOf(device); !uid.empty())
                ids.push_back(std::move(uid));
        }
        return ids;
    }

    AudioDeviceInfo describe(const std::string& id) override
    {
        AudioDeviceInfo info;
        info.id = id;
        const auto device = deviceFor(id);
        if (device == kAudioObjectUnknown)
        {
            info.answering = false;
            info.problem = "The audio device isn't there.";
            return info;
        }

        info.name = readText(device, address(kAudioObjectPropertyName));
        UInt32 transport = 0, domain = 0, alive = 1, block = 0;
        read(device, address(kAudioDevicePropertyTransportType), transport);
        read(device, address(kAudioDevicePropertyClockDomain), domain);
        read(device, address(kAudioDevicePropertyDeviceIsAlive), alive);
        info.transport = transportOf(transport);
        info.clockDomain = domain;

        const auto inputs = total(streamChannels(device, kAudioObjectPropertyScopeInput));
        const auto outputs = total(streamChannels(device, kAudioObjectPropertyScopeOutput));
        info.inputNames = channelNames(device, kAudioObjectPropertyScopeInput, inputs, "Input");
        info.outputNames = channelNames(device, kAudioObjectPropertyScopeOutput, outputs, "Output");

        // Opening a Bluetooth device's microphone switches it to its hands-free profile.
        info.handsFree = info.transport == AudioTransport::bluetooth && inputs > 0;
        info.loopback = info.transport == AudioTransport::virtualDevice;

        Float64 rate = 0.0;
        read(device, address(kAudioDevicePropertyNominalSampleRate), rate);
        info.currentRate = rate;
        std::vector<detail::Range> ranges;
        const auto bytes = readBytes(device, address(kAudioDevicePropertyAvailableNominalSampleRates));
        for (std::size_t at = 0; at + sizeof(AudioValueRange) <= bytes.size(); at += sizeof(AudioValueRange))
        {
            AudioValueRange range{};
            std::memcpy(&range, bytes.data() + at, sizeof(range));
            ranges.push_back({range.mMinimum, range.mMaximum});
        }
        info.sampleRates = detail::ratesWithin(ranges, rate);

        AudioValueRange sizes{};
        read(device, address(kAudioDevicePropertyBufferFrameSize), block);
        read(device, address(kAudioDevicePropertyBufferFrameSizeRange), sizes);
        info.currentBlock = static_cast<int>(block);
        info.blockSizes = detail::blockSizesWithin(sizes.mMinimum, sizes.mMaximum, info.currentBlock);

        if (alive == 0)
        {
            info.answering = false;
            info.problem = "The audio device has stopped.";
        }
        return info;
    }

    bool inputMuted() override
    {
        const auto mute = address(kAudioHardwarePropertyProcessInputMute);
        UInt32 muted = 0;
        return AudioObjectHasProperty(kAudioObjectSystemObject, &mute) && read(kAudioObjectSystemObject, mute, muted) &&
               muted != 0;
    }

    std::string defaultDevice(AudioDirection direction) override
    {
        AudioObjectID device = kAudioObjectUnknown;
        read(kAudioObjectSystemObject,
             address(direction == AudioDirection::input ? kAudioHardwarePropertyDefaultInputDevice
                                                        : kAudioHardwarePropertyDefaultOutputDevice),
             device);
        return device == kAudioObjectUnknown ? std::string() : uidOf(device);
    }

    std::unique_ptr<AudioSystemStream> open(const std::string& id, const AudioStreamRequest& request,
                                            AudioError& error) override
    {
        const auto device = deviceFor(id);
        if (device == kAudioObjectUnknown)
        {
            error = {"The audio device isn't there.", "open"};
            return nullptr;
        }

        // A rate or block size other than the device's, set, waited for and read back.
        if (request.sampleRate > 0.0 &&
            !setAndWait<Float64>(device, address(kAudioDevicePropertyNominalSampleRate), request.sampleRate, sameRate))
        {
            error = {"The audio device didn't change to that rate.", "set rate"};
            return nullptr;
        }
        if (request.blockSize > 0 &&
            !setAndWait<UInt32>(device, address(kAudioDevicePropertyBufferFrameSize),
                                static_cast<UInt32>(request.blockSize), sameFrames))
        {
            error = {"The audio device didn't change to that block size.", "set block size"};
            return nullptr;
        }

        // Core Audio gives a program the user hasn't allowed silence, so a denial is said here; one not yet asked is
        // asked as its input starts.
        const auto access = request.inputChannels.empty() ? MicrophoneAccess::allowed : microphoneAccess();
        if (access == MicrophoneAccess::denied)
        {
            error = {"This program isn't allowed to use the microphone. It can be allowed in System Settings, under "
                     "Privacy & Security, Microphone.",
                     "microphone permission"};
            return nullptr;
        }

        auto stream = std::make_unique<CoreAudioStream>(device, request, access == MicrophoneAccess::notAsked);
        if (!stream->prepare(error))
            return nullptr;
        return stream;
    }

    void addListener(AudioSystemListener& listener) override
    {
        bool first = false;
        {
            const std::lock_guard hold(listeners->lock);
            first = listeners->listeners.empty() && !listening;
            listeners->listeners.push_back(&listener);
        }
        if (first)
            startListening();
    }

    void removeListener(AudioSystemListener& listener) override
    {
        std::unique_lock hold(listeners->lock);
        std::erase(listeners->listeners, &listener);
        listeners->idle.wait(hold, [this] { return listeners->callsUnderWay == 0; });
    }

private:
    void startListening()
    {
        listening = true;
        const auto to = listeners;
        devicesBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { to->tell(AudioSystemChange::devices); });
        defaultsBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { to->tell(AudioSystemChange::defaults); });
        restartBlock =
            Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { to->tell(AudioSystemChange::serviceRestarted); });
        muteBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { to->tell(AudioSystemChange::inputMute); });
        const auto devices = address(kAudioHardwarePropertyDevices);
        const auto input = address(kAudioHardwarePropertyDefaultInputDevice);
        const auto output = address(kAudioHardwarePropertyDefaultOutputDevice);
        const auto restarted = address(kAudioHardwarePropertyServiceRestarted);
        const auto mute = address(kAudioHardwarePropertyProcessInputMute);
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &devices, queue, devicesBlock);
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &input, queue, defaultsBlock);
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &output, queue, defaultsBlock);
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &restarted, queue, restartBlock);
        // The process's input mute, from macOS 14.
        listeningToMute = AudioObjectHasProperty(kAudioObjectSystemObject, &mute) &&
                          AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &mute, queue, muteBlock) == noErr;

        // The computer's sleep and waking, from the power manager.
        power = std::make_unique<Power>();
        power->listeners = listeners;
        power->root = IORegisterForSystemPower(power.get(), &power->port, &CoreAudioSystem::powerChanged, &power->notifier);
        if (power->root != MACH_PORT_NULL)
            IONotificationPortSetDispatchQueue(power->port, queue);
    }

    void stopListening()
    {
        if (!listening)
            return;
        const auto devices = address(kAudioHardwarePropertyDevices);
        const auto input = address(kAudioHardwarePropertyDefaultInputDevice);
        const auto output = address(kAudioHardwarePropertyDefaultOutputDevice);
        const auto restarted = address(kAudioHardwarePropertyServiceRestarted);
        const auto mute = address(kAudioHardwarePropertyProcessInputMute);
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &devices, queue, devicesBlock);
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &input, queue, defaultsBlock);
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &output, queue, defaultsBlock);
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &restarted, queue, restartBlock);
        if (listeningToMute)
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &mute, queue, muteBlock);
        Block_release(devicesBlock);
        Block_release(defaultsBlock);
        Block_release(restartBlock);
        Block_release(muteBlock);
        if (power != nullptr && power->root != MACH_PORT_NULL)
        {
            IODeregisterForSystemPower(&power->notifier);
            IOServiceClose(power->root);
            IONotificationPortDestroy(power->port);
        }
        // Calls already queued finish before the queue is let go.
        dispatch_sync(queue, ^{
                      });
        power.reset();
        listening = false;
    }

    struct Power
    {
        std::shared_ptr<SystemListeners> listeners;
        io_connect_t root = MACH_PORT_NULL;
        IONotificationPortRef port = nullptr;
        io_object_t notifier = 0;
    };

    static void powerChanged(void* context, io_service_t, natural_t message, void* argument)
    {
        auto& power = *static_cast<Power*>(context);
        switch (message)
        {
            case kIOMessageSystemWillSleep:
                power.listeners->tell(AudioSystemChange::sleeping);
                IOAllowPowerChange(power.root, reinterpret_cast<long>(argument));
                break;
            case kIOMessageCanSystemSleep:
                IOAllowPowerChange(power.root, reinterpret_cast<long>(argument));
                break;
            case kIOMessageSystemHasPoweredOn:
                power.listeners->tell(AudioSystemChange::woke);
                break;
            default:
                break;
        }
    }

    dispatch_queue_t queue;
    std::shared_ptr<SystemListeners> listeners = std::make_shared<SystemListeners>();
    bool listening = false;
    AudioObjectPropertyListenerBlock devicesBlock = nullptr, defaultsBlock = nullptr, restartBlock = nullptr;
    AudioObjectPropertyListenerBlock muteBlock = nullptr;
    bool listeningToMute = false;
    std::unique_ptr<Power> power;
};
}

namespace devices::detail
{
std::vector<std::shared_ptr<AudioSystem>> systemsOfThisComputer()
{
    return {std::make_shared<CoreAudioSystem>()};
}
}

}
