#include "symbolengine/host/Vst3Format.h"

#include "Vst3Module.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace symbolengine
{

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{

// What the plugin is told the program is called.
class HostContext final : public HostApplication
{
public:
    explicit HostContext(std::string name) : programName(std::move(name)) {}

    tresult PLUGIN_API getName(String128 name) override
    {
        return StringConvert::convert(programName, name) ? kResultTrue : kInternalError;
    }

private:
    std::string programName;
};

// The count every host object a plugin is handed keeps, by COM's rules: it starts at one, as given to its maker.
class Counted
{
protected:
    uint32 addOne() { return ++references; }

    template <typename Self>
    uint32 releaseOne(Self* self)
    {
        const auto left = --references;

        if (left == 0)
            delete self;

        return left;
    }

private:
    std::atomic<uint32> references{1};
};

// What a plugin says to the host: each change only recorded, as a notice for the host module to act on from the main
// thread once the call it may have come from has returned; and once the plugin is gone, nothing.
class ComponentHandler final : public IComponentHandler, public IComponentHandler2, Counted
{
public:
    std::atomic<PluginFormatClient*> client{nullptr};

    tresult PLUGIN_API beginEdit(ParamID) override { return live(); }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return live(); }
    tresult PLUGIN_API endEdit(ParamID) override { return live(); }

    tresult PLUGIN_API restartComponent(int32 flags) override
    {
        auto* const listener = client.load();

        if (listener == nullptr)
            return kResultFalse;

        std::uint32_t what = 0;

        if ((flags & kLatencyChanged) != 0)
            what |= static_cast<std::uint32_t>(PluginNotice::latency);

        if ((flags & (kIoChanged | kIoTitlesChanged)) != 0)
            what |= static_cast<std::uint32_t>(PluginNotice::buses);

        if ((flags & (kParamTitlesChanged | kParamIDMappingChanged)) != 0)
            what |= static_cast<std::uint32_t>(PluginNotice::parameters);

        if ((flags & kParamValuesChanged) != 0)
            what |= static_cast<std::uint32_t>(PluginNotice::values);

        if ((flags & kReloadComponent) != 0)
            what |= static_cast<std::uint32_t>(PluginNotice::reload);

        if (what != 0)
            listener->notice(static_cast<PluginNotice>(what));

        return kResultOk;
    }

    tresult PLUGIN_API setDirty(TBool dirty) override
    {
        if (auto* const listener = client.load(); listener != nullptr && dirty)
            listener->notice(PluginNotice::state);

        return live();
    }

    tresult PLUGIN_API requestOpenEditor(FIDString) override { return kResultFalse; }
    tresult PLUGIN_API startGroupEdit() override { return kNotImplemented; }
    tresult PLUGIN_API finishGroupEdit() override { return kNotImplemented; }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(wanted, IComponentHandler::iid))
            *object = static_cast<IComponentHandler*>(this);
        else if (FUnknownPrivate::iidEqual(wanted, IComponentHandler2::iid))
            *object = static_cast<IComponentHandler2*>(this);
        else
        {
            *object = nullptr;
            return kNoInterface;
        }

        addRef();
        return kResultOk;
    }

    uint32 PLUGIN_API addRef() override { return addOne(); }
    uint32 PLUGIN_API release() override { return releaseOne(this); }

private:
    tresult live() const { return client.load() != nullptr ? kResultOk : kResultFalse; }
};

// One direction of the link between a plugin's two parts: a message sent from any thread reaches the other part on
// the main thread, kept until then, and nothing reaches it once the link is undone.
class ConnectionProxy final : public IConnectionPoint, Counted
{
public:
    ConnectionProxy(IConnectionPoint* to, std::shared_ptr<Vst3Format::Shared> owner);

    tresult PLUGIN_API connect(IConnectionPoint*) override { return kResultFalse; }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API notify(IMessage* message) override;

    void undo() { linked = false; }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(wanted, IConnectionPoint::iid))
        {
            *object = static_cast<IConnectionPoint*>(this);
            addRef();
            return kResultOk;
        }

        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return addOne(); }
    uint32 PLUGIN_API release() override { return releaseOne(this); }

private:
    IPtr<IConnectionPoint> destination;
    std::shared_ptr<Vst3Format::Shared> shared;
    std::atomic<bool> linked{true};
};

// The stream a plugin's state is written to and read from: it takes null out-pointers, counts exactly, never reads
// past its end or before its start, and says its size.
class HostStream final : public IBStream, public ISizeableStream, Counted
{
public:
    std::string bytes;

    tresult PLUGIN_API read(void* buffer, int32 count, int32* read) override
    {
        if (count < 0 || (buffer == nullptr && count > 0))
            return kInvalidArgument;

        const auto available = static_cast<int64>(bytes.size()) - at;
        const auto taken = static_cast<int32>(std::clamp<int64>(available, 0, count));

        if (taken > 0)
            std::memcpy(buffer, bytes.data() + at, static_cast<std::size_t>(taken));

        at += taken;

        if (read != nullptr)
            *read = taken;

        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 count, int32* written) override
    {
        if (count < 0 || (buffer == nullptr && count > 0))
            return kInvalidArgument;

        const auto end = static_cast<std::size_t>(at) + static_cast<std::size_t>(count);

        if (end > bytes.size())
            bytes.resize(end);

        if (count > 0)
            std::memcpy(bytes.data() + at, buffer, static_cast<std::size_t>(count));

        at += count;

        if (written != nullptr)
            *written = count;

        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 position, int32 mode, int64* result) override
    {
        const int64 from = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? at : static_cast<int64>(bytes.size());

        if (mode != kIBSeekSet && mode != kIBSeekCur && mode != kIBSeekEnd)
            return kInvalidArgument;

        at = std::clamp<int64>(from + position, 0, static_cast<int64>(bytes.size()));

        if (result != nullptr)
            *result = at;

        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* position) override
    {
        if (position == nullptr)
            return kInvalidArgument;

        *position = at;
        return kResultOk;
    }

    tresult PLUGIN_API getStreamSize(int64& size) override
    {
        size = static_cast<int64>(bytes.size());
        return kResultOk;
    }

    tresult PLUGIN_API setStreamSize(int64 size) override
    {
        if (size < 0)
            return kInvalidArgument;

        bytes.resize(static_cast<std::size_t>(size));
        at = std::min(at, size);
        return kResultOk;
    }

    void rewind() { at = 0; }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IBStream::iid))
            *object = static_cast<IBStream*>(this);
        else if (FUnknownPrivate::iidEqual(wanted, ISizeableStream::iid))
            *object = static_cast<ISizeableStream*>(this);
        else
        {
            *object = nullptr;
            return kNoInterface;
        }

        addRef();
        return kResultOk;
    }

    uint32 PLUGIN_API addRef() override { return addOne(); }
    uint32 PLUGIN_API release() override { return releaseOne(this); }

private:
    int64 at = 0;
};

// A plugin's state as the host keeps it: its component's and its controller's, apart, each said present or not.
constexpr char stateMagic[4] = {'S', 'E', 'V', '3'};

std::string packState(const std::string* component, const std::string* controller)
{
    std::string out(stateMagic, 4);
    out += static_cast<char>(1);   // the form's version
    out += static_cast<char>((component != nullptr ? 1 : 0) | (controller != nullptr ? 2 : 0));

    for (const auto* part : {component, controller})
    {
        const auto size = part != nullptr ? static_cast<std::uint64_t>(part->size()) : 0;

        for (int i = 0; i < 8; ++i)
            out += static_cast<char>((size >> (8 * i)) & 0xFF);

        if (part != nullptr)
            out += *part;
    }

    return out;
}

// The parts of a state in the host's form; a state in no form of the host's is taken as the component's alone.
bool unpackState(std::string_view state, std::optional<std::string>& component, std::optional<std::string>& controller)
{
    if (state.size() < 6 || std::memcmp(state.data(), stateMagic, 4) != 0)
    {
        component = std::string(state);
        return true;
    }

    const auto present = static_cast<unsigned char>(state[5]);
    std::size_t at = 6;

    for (auto* part : {&component, &controller})
    {
        if (state.size() < at + 8)
            return false;

        std::uint64_t size = 0;

        for (int i = 0; i < 8; ++i)
            size |= static_cast<std::uint64_t>(static_cast<unsigned char>(state[at + static_cast<std::size_t>(i)]))
                    << (8 * i);

        at += 8;

        if (state.size() - at < size)
            return false;

        if ((present & (part == &component ? 1 : 2)) != 0)
            *part = std::string(state.substr(at, static_cast<std::size_t>(size)));

        at += static_cast<std::size_t>(size);
    }

    return true;
}

SpeakerArrangement arrangementFor(int channels)
{
    return channels == 1 ? SpeakerArr::kMono : channels == 2 ? SpeakerArr::kStereo : SpeakerArr::kEmpty;
}

}

struct Vst3Format::Shared : std::enable_shared_from_this<Vst3Format::Shared>
{
    Options options;
    IPtr<HostContext> context;

    // The modules this format has used, kept for as long as it lives, each given the host's context once.
    std::map<std::string, std::shared_ptr<Vst3Module>> modules;

    std::shared_ptr<Vst3Module> module(const std::filesystem::path& location, PluginError& error)
    {
        const auto key = location.generic_string();

        if (const auto found = modules.find(key); found != modules.end())
            return found->second;

        auto opened = Vst3Module::open(location, error);

        if (opened == nullptr)
            return nullptr;

        // Before any class is looked up: a module holding many plugins can number them differently once it has it.
        if (FUnknownPtr<IPluginFactory3> factory(opened->factory()); factory)
            factory->setHostContext(context);

        modules[key] = opened;
        return opened;
    }
};

ConnectionProxy::ConnectionProxy(IConnectionPoint* to, std::shared_ptr<Vst3Format::Shared> owner)
    : destination(to), shared(std::move(owner))
{
}

tresult PLUGIN_API ConnectionProxy::notify(IMessage* message)
{
    if (!linked || message == nullptr)
        return kResultFalse;

    if (shared->options.onMainThread())
        return destination->notify(message);

    // Kept, and handed over on the main thread.
    IPtr<IMessage> kept(message);
    IPtr<ConnectionProxy> self(this);
    shared->options.post([self, kept] {
        if (self->linked)
            self->destination->notify(kept);
    });
    return kResultOk;
}

namespace
{

class Vst3Plugin final : public PluginFormatInstance
{
public:
    Vst3Plugin(std::shared_ptr<Vst3Format::Shared> owner, std::shared_ptr<Vst3Module> code)
        : shared(std::move(owner)), module(std::move(code)), handler(owned(new ComponentHandler))
    {
        ++module->plugins;
    }

    // Made from its factory, initialized, its controller found or made, the two parts linked and the controller given
    // the component's state, as a plugin expects of its host.
    bool make(const TUID classId, PluginFormatClient& client, PluginError& error)
    {
        handler->client = &client;
        IComponent* made = nullptr;

        if (module->factory()->createInstance(classId, IComponent::iid, reinterpret_cast<void**>(&made)) != kResultOk ||
            made == nullptr)
        {
            error = {"It couldn't be made from its module.", "createInstance", 0};
            return false;
        }

        component = owned(made);

        if (const auto result = component->initialize(shared->context); result != kResultOk)
        {
            component = nullptr;
            error = {"It refused to start.", "initialize", result};
            return false;
        }

        initialized = true;
        processor = FUnknownPtr<IAudioProcessor>(component);

        if (!processor)
        {
            error = {"It doesn't process audio.", "initialize", 0};
            return false;
        }

        // One object with both parts, or a controller apart: told apart by the objects' own identity, never by the
        // pointers the query gave.
        if (FUnknownPtr<IEditController> both(component); both)
        {
            controller = both;
            FUnknownPtr<FUnknown> one(component), other(controller);
            separate = one.get() != other.get();
        }
        else if (TUID controllerId{}; component->getControllerClassId(controllerId) == kResultTrue)
        {
            IEditController* madeController = nullptr;

            if (module->factory()->createInstance(controllerId, IEditController::iid,
                                                  reinterpret_cast<void**>(&madeController)) == kResultOk &&
                madeController != nullptr)
            {
                controller = owned(madeController);
                separate = true;
            }
        }

        if (controller && separate)
        {
            if (controller->initialize(shared->context) != kResultOk)
                controller = nullptr;
            else
                controllerInitialized = true;
        }

        if (!controller)
            return true;

        controller->setComponentHandler(handler);

        if (!separate)
            return true;

        // The component to the controller first, then back, each through a proxy that hands messages over on the
        // main thread.
        componentPoint = FUnknownPtr<IConnectionPoint>(component);
        controllerPoint = FUnknownPtr<IConnectionPoint>(controller);

        if (componentPoint && controllerPoint)
        {
            toController = owned(new ConnectionProxy(controllerPoint, shared));
            toComponent = owned(new ConnectionProxy(componentPoint, shared));
            componentPoint->connect(toController);
            controllerPoint->connect(toComponent);
        }

        HostStream* stream = new HostStream;
        IPtr<HostStream> held = owned(stream);

        if (component->getState(stream) == kResultOk)
        {
            stream->rewind();
            controller->setComponentState(stream);
        }

        return true;
    }

    ~Vst3Plugin() override
    {
        // Taken apart in order: links undone, event buses switched off, the handler let go of, the controller ended,
        // the processor let go of, the component ended.
        if (toController)
        {
            toController->undo();
            toComponent->undo();
            componentPoint->disconnect(toController);
            controllerPoint->disconnect(toComponent);
        }

        if (component && initialized)
            for (const auto direction : {kInput, kOutput})
                for (int32 i = 0; i < component->getBusCount(kEvent, direction); ++i)
                    component->activateBus(kEvent, direction, i, false);

        if (controller)
            controller->setComponentHandler(nullptr);

        handler->client = nullptr;

        if (controller && separate && controllerInitialized)
            controller->terminate();

        controller = nullptr;
        processor = nullptr;

        if (component && initialized)
            component->terminate();

        component = nullptr;
        --module->plugins;
    }

    PluginLayout arrange(PluginLayout wanted) override
    {
        const auto mainBus = [this](BusDirection direction, int32 count) {
            for (int32 i = 0; i < count; ++i)
                if (BusInfo info{};
                    component->getBusInfo(kAudio, direction, i, info) == kResultOk && info.busType == kMain)
                    return i;

            return count > 0 ? 0 : -1;
        };

        // Asked with an entry for every bus, never a null array, the main buses as wanted and the others as they are;
        // what the plugin settled on is read back after, whatever it answered, since some describe their buses only once
        // asked for an arrangement.
        for (const auto direction : {kInput, kOutput})
            counts[direction] = component->getBusCount(kAudio, direction);

        const int32 mainIn = mainBus(kInput, counts[kInput]), mainOut = mainBus(kOutput, counts[kOutput]);
        std::vector<SpeakerArrangement> ins(static_cast<std::size_t>(std::max(1, counts[kInput])), SpeakerArr::kEmpty),
            outs(static_cast<std::size_t>(std::max(1, counts[kOutput])), SpeakerArr::kEmpty);

        for (int32 i = 0; i < counts[kInput]; ++i)
            if (i == mainIn)
                ins[static_cast<std::size_t>(i)] = arrangementFor(wanted.inputs);
            else
                processor->getBusArrangement(kInput, i, ins[static_cast<std::size_t>(i)]);

        for (int32 i = 0; i < counts[kOutput]; ++i)
            if (i == mainOut)
                outs[static_cast<std::size_t>(i)] = arrangementFor(wanted.outputs);
            else
                processor->getBusArrangement(kOutput, i, outs[static_cast<std::size_t>(i)]);

        processor->setBusArrangements(ins.data(), counts[kInput], outs.data(), counts[kOutput]);

        for (const auto direction : {kInput, kOutput})
        {
            counts[direction] = component->getBusCount(kAudio, direction);
            main[direction] = mainBus(direction, counts[direction]);
            channels[direction].assign(static_cast<std::size_t>(std::max(0, counts[direction])), 0);

            for (int32 i = 0; i < counts[direction]; ++i)
            {
                BusInfo info{};
                SpeakerArrangement arrangement = SpeakerArr::kEmpty;

                if (component->getBusInfo(kAudio, direction, i, info) == kResultOk)
                    channels[direction][static_cast<std::size_t>(i)] = std::max(0, info.channelCount);
                else if (processor->getBusArrangement(direction, i, arrangement) == kResultOk)
                    channels[direction][static_cast<std::size_t>(i)] = SpeakerArr::getChannelCount(arrangement);

                // The main buses on, the others off, as nothing feeds them.
                component->activateBus(kAudio, direction, i, i == main[direction]);
            }

            for (int32 i = 0; i < component->getBusCount(kEvent, direction); ++i)
                component->activateBus(kEvent, direction, i, true);
        }

        const auto mainChannels = [this](BusDirection direction) {
            return main[direction] >= 0 ? channels[direction][static_cast<std::size_t>(main[direction])] : 0;
        };

        return {mainChannels(kInput), mainChannels(kOutput)};
    }

    bool activate(const PluginSetup& setup, PluginError& error) override
    {
        processSetup = {setup.offline ? kOffline : kRealtime, kSample32, setup.maximumFrames, setup.rate};

        if (const auto result = processor->setupProcessing(processSetup); result != kResultOk)
        {
            error = {"It refused to be set up for this rate and block size.", "setupProcessing", result};
            return false;
        }

        // A buffer for every bus, the main buses' filled each block, the others silence in and scratch space out.
        silence.assign(static_cast<std::size_t>(std::max(1, setup.maximumFrames)), 0.0f);

        for (const auto direction : {kInput, kOutput})
        {
            auto& buses = busBuffers[direction];
            auto& pointers = channelPointers[direction];
            buses.assign(static_cast<std::size_t>(std::max(0, counts[direction])), AudioBusBuffers{});
            pointers.assign(buses.size(), {});
            scratch[direction].assign(buses.size(), {});

            for (std::size_t i = 0; i < buses.size(); ++i)
            {
                const auto count = static_cast<std::size_t>(channels[direction][i]);
                pointers[i].assign(std::max<std::size_t>(1, count), silence.data());

                if (direction == kOutput && static_cast<int32>(i) != main[direction])
                {
                    scratch[direction][i].assign(count, std::vector<float>(silence.size(), 0.0f));

                    for (std::size_t c = 0; c < count; ++c)
                        pointers[i][c] = scratch[direction][i][c].data();
                }

                buses[i].numChannels = static_cast<int32>(count);
                buses[i].silenceFlags = 0;
                buses[i].channelBuffers32 = pointers[i].data();
            }
        }

        context = {};
        context.sampleRate = setup.rate;
        context.state = ProcessContext::kContTimeValid | ProcessContext::kSystemTimeValid;

        if (const auto result = component->setActive(true); result != kResultOk)
        {
            error = {"It refused to start processing.", "setActive", result};
            return false;
        }

        return true;
    }

    void deactivate() override { component->setActive(false); }

    int latency() override { return static_cast<int>(std::min<uint32>(processor->getLatencySamples(), INT_MAX)); }

    int tail() override { return static_cast<int>(std::min<uint32>(processor->getTailSamples(), INT_MAX)); }

    // VST3's reset is processing stopped and started again, which the host module does around this.
    void reset() override {}

    void startProcessing() noexcept override { processor->setProcessing(true); }

    void stopProcessing() noexcept override { processor->setProcessing(false); }

    bool process(const PluginBlock& block) noexcept override
    {
        auto& ins = channelPointers[kInput];
        auto& outs = channelPointers[kOutput];

        if (main[kInput] >= 0)
            for (int c = 0; c < block.inputChannels; ++c)
                ins[static_cast<std::size_t>(main[kInput])][static_cast<std::size_t>(c)] =
                    const_cast<float*>(block.inputs[c]);

        if (main[kOutput] >= 0)
            for (int c = 0; c < block.outputChannels; ++c)
                outs[static_cast<std::size_t>(main[kOutput])][static_cast<std::size_t>(c)] = block.outputs[c];

        return run(block.frames);
    }

    // Values handed over with no audio: a call with no samples and every bus's buffers, never null ones.
    void flush() override { run(0); }

    std::optional<std::string> state(PluginStateUse, PluginError& error) override
    {
        IPtr<HostStream> componentState = owned(new HostStream);
        const auto result = component->getState(componentState);

        if (result != kResultOk && result != kNotImplemented)
        {
            error = {"It gave no state.", "getState", result};
            return std::nullopt;
        }

        IPtr<HostStream> controllerState = owned(new HostStream);
        const bool controllerGave = controller && separate && controller->getState(controllerState) == kResultOk;

        return packState(result == kResultOk ? &componentState->bytes : nullptr,
                         controllerGave ? &controllerState->bytes : nullptr);
    }

    bool setState(std::string_view state, PluginStateUse, PluginError& error) override
    {
        std::optional<std::string> componentPart, controllerPart;

        if (!unpackState(state, componentPart, controllerPart))
        {
            error = {"The state is damaged.", "setState", 0};
            return false;
        }

        // The component's, the same bytes from their start to the controller, then the controller's own.
        if (componentPart.has_value() && !componentPart->empty())
        {
            IPtr<HostStream> stream = owned(new HostStream);
            stream->bytes = *componentPart;

            if (const auto result = component->setState(stream); result != kResultOk)
            {
                error = {"It didn't take its state.", "setState", result};
                return false;
            }

            if (controller && separate)
            {
                stream->rewind();
                controller->setComponentState(stream);
            }
        }

        if (controllerPart.has_value() && !controllerPart->empty() && controller && separate)
        {
            IPtr<HostStream> stream = owned(new HostStream);
            stream->bytes = *controllerPart;
            controller->setState(stream);
        }

        return true;
    }

private:
    bool run(int frames) noexcept
    {
        inputChanges.clearQueue();
        outputChanges.clearQueue();
        inputEvents.clear();
        outputEvents.clear();

        ProcessData data;
        data.processMode = processSetup.processMode;
        data.symbolicSampleSize = kSample32;
        data.numSamples = frames;
        data.numInputs = counts[kInput];
        data.numOutputs = counts[kOutput];
        data.inputs = busBuffers[kInput].empty() ? nullptr : busBuffers[kInput].data();
        data.outputs = busBuffers[kOutput].empty() ? nullptr : busBuffers[kOutput].data();
        data.inputParameterChanges = &inputChanges;
        data.outputParameterChanges = &outputChanges;
        data.inputEvents = &inputEvents;
        data.outputEvents = &outputEvents;
        data.processContext = &context;

        context.systemTime =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const bool fine = processor->process(data) == kResultOk;
        context.continousTimeSamples += frames;
        return fine;
    }

    std::shared_ptr<Vst3Format::Shared> shared;
    std::shared_ptr<Vst3Module> module;

    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    bool separate = false, initialized = false, controllerInitialized = false;
    IPtr<ComponentHandler> handler;
    IPtr<IConnectionPoint> componentPoint, controllerPoint;
    IPtr<ConnectionProxy> toController, toComponent;

    // By direction, kInput and kOutput: how many audio buses, the main one's index, each bus's channels.
    int32 counts[2] = {0, 0};
    int32 main[2] = {-1, -1};
    std::vector<int32> channels[2];

    ProcessSetup processSetup{};
    std::vector<AudioBusBuffers> busBuffers[2];
    std::vector<std::vector<Sample32*>> channelPointers[2];
    std::vector<std::vector<std::vector<float>>> scratch[2];
    std::vector<float> silence;
    ParameterChanges inputChanges, outputChanges;
    EventList inputEvents, outputEvents;
    ProcessContext context{};
};

}

Vst3Format::Vst3Format(Options options) : shared(std::make_shared<Shared>())
{
    shared->options = std::move(options);
    shared->context = owned(new HostContext(shared->options.hostName));
}

Vst3Format::~Vst3Format()
{
    // The factories let go of this format's host context; the modules stay loaded, and are exited when the program
    // ends.
    for (const auto& [location, module] : shared->modules)
        if (FUnknownPtr<IPluginFactory3> factory(module->factory()); factory)
            factory->setHostContext(nullptr);

    shared->modules.clear();
}

PluginDescription Vst3Format::describe(const std::filesystem::path& module, const std::string& classId,
                                       std::string name)
{
    PluginDescription plugin;
    plugin.identity = {"VST3", classId};
    plugin.location = module;
    plugin.name = name.empty() ? module.stem().string() : std::move(name);
    return plugin;
}

std::string Vst3Format::name() const
{
    return "VST3";
}

void Vst3Format::make(const PluginDescription& plugin, PluginFormatClient& client,
                      std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done)
{
    PluginError error;
    FUID id;

    if (plugin.identity.id.size() != 32 || !id.fromString(plugin.identity.id.c_str()))
    {
        done(nullptr, {"Its identity isn't a VST3 class ID.", "make", 0});
        return;
    }

    auto module = shared->module(plugin.location, error);

    if (module == nullptr)
    {
        done(nullptr, error);
        return;
    }

    TUID classId;
    id.toTUID(classId);
    auto made = std::make_unique<Vst3Plugin>(shared, std::move(module));

    if (!made->make(classId, client, error))
    {
        done(nullptr, error);
        return;
    }

    done(std::move(made), {});
}

}
