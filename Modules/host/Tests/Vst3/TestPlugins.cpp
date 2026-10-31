// The test module's plugins, written straight against Steinberg's SDK. Each checks the calls a host makes into it, as
// Steinberg's host checker does, and writes what was wrong to the file SYMBOLENGINE_TEST_VST3_LOG names; most also
// misbehave one way, as plugins do.

#include "TestEditor.h"
#include "TestIds.h"

// First, before anything that includes the controller's interface: it renames the controller's state calls while it
// includes it, as the SDK's own source sees it, so the classes here are laid out as the SDK's are.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/main/moduleinit.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <os/lock.h>
    #include <pthread.h>
#elif defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <objbase.h>
#else
    #include <pthread.h>
#endif

// A file every system has, which the unsafe plugins open.
#if defined(_WIN32)
    #define SYMBOLENGINE_TEST_NOWHERE "NUL"
    #define SYMBOLENGINE_TEST_NOINLINE __declspec(noinline)
#else
    #define SYMBOLENGINE_TEST_NOWHERE "/dev/null"
    #define SYMBOLENGINE_TEST_NOINLINE __attribute__((noinline))
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{

void misuse(const char* plugin, const std::string& what)
{
    static std::mutex lock;
    const std::lock_guard held(lock);

    if (const char* path = std::getenv("SYMBOLENGINE_TEST_VST3_LOG"))
        if (FILE* log = std::fopen(path, "ab"))
        {
            std::fprintf(log, "%s: %s\n", plugin, what.c_str());
            std::fclose(log);
        }
}

// The module's own entry and exit, written to the file SYMBOLENGINE_TEST_VST3_LIFE names: each is to come once.
void life(const char* what)
{
    if (const char* path = std::getenv("SYMBOLENGINE_TEST_VST3_LIFE"))
        if (FILE* log = std::fopen(path, "ab"))
        {
            std::fprintf(log, "%s\n", what);
            std::fclose(log);
        }
}

const ModuleInitializer entered([] { life("entered"); });
const ModuleTerminator exited([] { life("exited"); });

// A static object of its own, which the module's exit may use, so it is to go only after that.
struct OwnStatic
{
    ~OwnStatic() { life("taken down"); }
};
const OwnStatic ownStatic;

// What a processor checks of its host: every call but process and setProcessing on the thread it was started on, in
// the order the SDK's call sequence allows, and every block's buffers as its buses say.
// What the host checker asks of a process call's context: one in every call, at the rate the plugin was set up at, its
// system time rising and its count of processed samples running on without a jump.
struct ContextClock
{
    int64 lastSystemTime = 0;
    int64 nextContinuous = -1;
};

void checkContext(const char* plugin, const ProcessData& data, const ProcessSetup& setup, ContextClock& clock)
{
    if (data.processContext == nullptr)
    {
        misuse(plugin, "given no process context");
        return;
    }

    const auto& context = *data.processContext;

    if (context.sampleRate != setup.sampleRate)
        misuse(plugin, "given a context at another rate than it was set up at");

    if ((context.state & ProcessContext::kSystemTimeValid) != 0)
    {
        if (context.systemTime <= clock.lastSystemTime)
            misuse(plugin, "given a system time that didn't rise");

        clock.lastSystemTime = context.systemTime;
    }

    if ((context.state & ProcessContext::kContTimeValid) != 0)
    {
        if (clock.nextContinuous >= 0 && context.continousTimeSamples != clock.nextContinuous)
            misuse(plugin, "given a count of processed samples that jumped");

        clock.nextContinuous = context.continousTimeSamples + data.numSamples;
    }
}

class Checked : public AudioEffect
{
public:
    explicit Checked(const char* name, const FUID& controller = {}) : plugin(name)
    {
        if (controller.isValid())
            setControllerClass(controller);
    }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        mainThread = std::this_thread::get_id();

        if (++initializations > 1)
            misuse(plugin, "initialized twice");

        const auto result = AudioEffect::initialize(context);

        if (result == kResultOk)
            addBuses();

        return result;
    }

    tresult PLUGIN_API terminate() override
    {
        onMain("terminate");

        if (active)
            misuse(plugin, "terminated while active");

        // Whatever else of the host's held it, its links to the controller above all, has let go by now, and the one
        // hold left is what is ending it.
        if (getRefCount() != 1)
            misuse(plugin, "terminated while the host held it " + std::to_string(getRefCount()) + " times");

        return AudioEffect::terminate();
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 ins, SpeakerArrangement* outputs,
                                          int32 outs) override
    {
        onMain("setBusArrangements");

        if (active)
            misuse(plugin, "arranged while active");

        if ((ins > 0 && inputs == nullptr) || (outs > 0 && outputs == nullptr))
            misuse(plugin, "given a null arrangement");

        if (ins != static_cast<int32>(audioInputs.size()) || outs != static_cast<int32>(audioOutputs.size()))
            misuse(plugin, "given arrangements for another number of buses");

        arranged = true;
        return arrange(inputs, ins, outputs, outs);
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override
    {
        onMain("setupProcessing");

        if (active)
            misuse(plugin, "set up while active");

        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) override
    {
        onMain("setActive");

        if (processing)
            misuse(plugin, "activated or deactivated while processing");

        if (static_cast<bool>(state) == active)
            misuse(plugin, "activated or deactivated twice");

        active = state;
        return AudioEffect::setActive(state);
    }

    tresult PLUGIN_API setProcessing(TBool state) override
    {
        if (!active)
            misuse(plugin, "processing started or stopped while inactive");

        processing = state;
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (!processing && data.numSamples > 0)
            misuse(plugin, "processed while not processing");

        if (data.numSamples > processSetup.maxSamplesPerBlock)
            misuse(plugin, "given more samples than it was set up for");

        if (data.numInputs != static_cast<int32>(audioInputs.size()) ||
            data.numOutputs != static_cast<int32>(audioOutputs.size()))
            misuse(plugin, "given buffers for another number of buses");

        for (int32 i = 0; i < data.numInputs; ++i)
            check(data.inputs[i], audioInputs[static_cast<std::size_t>(i)]);

        checkContext(plugin, data, processSetup, clock);

        for (int32 i = 0; i < data.numOutputs; ++i)
            check(data.outputs[i], audioOutputs[static_cast<std::size_t>(i)]);

        // Each parameter's changes in a queue of their own, at frames within the block, in order.
        gainChanges = nullptr;

        if (data.inputParameterChanges != nullptr)
            for (int32 q = 0; q < data.inputParameterChanges->getParameterCount(); ++q)
                if (auto* changes = data.inputParameterChanges->getParameterData(q); changes != nullptr)
                {
                    checkQueue(*changes, data.numSamples);

                    if (changes->getParameterId() == testIds::gainParameter)
                        gainChanges = changes;

                    int32 offset = 0;
                    ParamValue value = 0.0;

                    if (changes->getParameterId() == testIds::messageParameter && changes->getPointCount() > 0 &&
                        changes->getPoint(changes->getPointCount() - 1, offset, value) == kResultTrue && value > 0.25)
                        for (int i = 0; i < 2; ++i)
                            if (IPtr<IMessage> message = owned(allocateMessage()))
                            {
                                message->setMessageID(value > 0.75 ? "crash" : "throw");
                                sendMessage(message);
                            }
                }

        if (data.numSamples > 0 && data.numInputs > 0 && data.numOutputs > 0)
        {
            play(data);

            // Its output's peak, reported as its meter.
            int32 index = 0;

            if (data.outputParameterChanges != nullptr)
                if (auto* meter = data.outputParameterChanges->addParameterData(testIds::meterParameter, index))
                    meter->addPoint(0, peak, index);
        }
        else if (gainChanges != nullptr && gainChanges->getPointCount() > 0)
        {
            int32 offset = 0;
            ParamValue value = 0.0;
            gainChanges->getPoint(gainChanges->getPointCount() - 1, offset, value);
            gain = static_cast<float>(value);
        }

        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) override
    {
        onMain("setState");
        int32 value = 0;
        float level = 0.0f;
        int32 read = 0;

        if (state != nullptr && state->read(&value, sizeof value, nullptr) == kResultOk)
            stored = value;

        if (state != nullptr && state->read(&level, sizeof level, &read) == kResultOk && read == sizeof level)
            gain = level;

        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) override
    {
        onMain("getState");
        int32 value = stored;
        float level = gain;

        if (state == nullptr)
            return kInvalidArgument;

        state->write(&value, sizeof value, nullptr);
        return state->write(&level, sizeof level, nullptr);
    }

protected:
    virtual void addBuses()
    {
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    }

    virtual tresult arrange(SpeakerArrangement* inputs, int32 ins, SpeakerArrangement* outputs, int32 outs)
    {
        return AudioEffect::setBusArrangements(inputs, ins, outputs, outs);
    }

    // The input at its gain, half to begin with, changing at the frame of each change to it, channel for channel, on
    // the main buses.
    virtual void play(ProcessData& data)
    {
        const auto& in = data.inputs[0];
        auto& out = data.outputs[0];
        const int32 points = gainChanges != nullptr ? gainChanges->getPointCount() : 0;
        int32 next = 0;
        float level = gain;
        peak = 0.0f;

        for (int32 i = 0; i < data.numSamples; ++i)
        {
            for (int32 offset = 0; next < points; ++next)
            {
                ParamValue value = 0.0;
                gainChanges->getPoint(next, offset, value);

                if (offset > i)
                    break;

                level = static_cast<float>(value);
            }

            for (int32 c = 0; c < out.numChannels; ++c)
            {
                out.channelBuffers32[c][i] =
                    in.numChannels > 0 ? level * in.channelBuffers32[std::min(c, in.numChannels - 1)][i] : 0.0f;
                peak = std::max(peak, std::abs(out.channelBuffers32[c][i]));
            }
        }

        gain = level;
    }

    // A queue's points lie within the block, the first frame for a call with no samples, in order.
    void checkQueue(IParamValueQueue& changes, int32 frames) const
    {
        int32 last = -1;

        for (int32 p = 0; p < changes.getPointCount(); ++p)
        {
            int32 offset = 0;
            ParamValue value = 0.0;

            if (changes.getPoint(p, offset, value) != kResultOk)
                misuse(plugin, "given a point it couldn't read");

            if (offset < 0 || offset >= std::max(1, frames) || offset <= last)
                misuse(plugin, "given a change outside its block or out of order");

            if (!(value >= 0.0 && value <= 1.0))
                misuse(plugin, "given a value outside 0 to 1");

            last = offset;
        }
    }

    std::atomic<float> gain{0.5f};
    float peak = 0.0f;
    IParamValueQueue* gainChanges = nullptr;
    ContextClock clock;

    void onMain(const char* call) const
    {
        if (std::this_thread::get_id() != mainThread)
            misuse(plugin, std::string(call) + " called off the thread it was started on");
    }

    const char* plugin;
    bool arranged = false;

private:
    void check(const AudioBusBuffers& buffers, const IPtr<Bus>& bus) const
    {
        auto* const audio = FCast<AudioBus>(bus.get());

        if (audio != nullptr && buffers.numChannels != SpeakerArr::getChannelCount(audio->getArrangement()))
            misuse(plugin, "given buffers for another number of channels than its bus has");

        if (buffers.numChannels > 0 && buffers.channelBuffers32 == nullptr)
            misuse(plugin, "given null buffers behind a channel count");

        for (int32 c = 0; c < buffers.numChannels && buffers.channelBuffers32 != nullptr; ++c)
            if (buffers.channelBuffers32[c] == nullptr)
                misuse(plugin, "given a null channel");

        if (buffers.silenceFlags != 0)
            misuse(plugin, "told its input is silent");
    }

    std::thread::id mainThread;
    std::atomic<int> initializations{0};
    std::atomic<bool> active{false}, processing{false};
    int32 stored = 0;
};

// Well behaved, with a controller of its own: half the input.
class Gain : public Checked
{
public:
    Gain() : Checked("Gain", FUID(SYMBOLENGINE_TEST_ID(0x02))) {}
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Gain); }
};

class GainController : public EditController
{
public:
    static FUnknown* make(void*) { return static_cast<IEditController*>(new GainController); }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = EditController::initialize(context);

        if (result != kResultOk)
            return result;

        parameters.addParameter(STR16("Gain"), STR16("x"), 0, 0.5, ParameterInfo::kCanAutomate, testIds::gainParameter);
        parameters.addParameter(STR16("Hidden"), nullptr, 0, 0.0, ParameterInfo::kIsHidden, testIds::hiddenParameter);
        parameters.addParameter(STR16("Meter"), nullptr, 0, 0.0, ParameterInfo::kIsReadOnly, testIds::meterParameter);
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                                ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, testIds::bypassParameter);
        parameters.addParameter(STR16("Program"), nullptr, 3, 0.0, ParameterInfo::kIsProgramChange,
                                testIds::programParameter);
        parameters.addParameter(STR16("Edit"), nullptr, 0, 0.0, ParameterInfo::kIsHidden, testIds::editParameter);
        parameters.addParameter(STR16("Dirty"), nullptr, 0, 0.0, ParameterInfo::kIsHidden, testIds::dirtyParameter);
        parameters.addParameter(STR16("Echo"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate, testIds::echoParameter);
        parameters.addParameter(STR16("Rounded echo"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate,
                                testIds::roundedEchoParameter);
        parameters.addParameter(STR16("Message"), nullptr, 0, 0.0, ParameterInfo::kIsHidden, testIds::messageParameter);
        return kResultOk;
    }

    // A message from its processor: one asking it to throw or crash does, as a controller that fails on what its
    // processor sends; one reaching it after that is wrong, as nothing should reach a plugin that has failed.
    tresult PLUGIN_API notify(IMessage* message) override
    {
        if (message == nullptr)
            return kInvalidArgument;

        if (failed)
            misuse("Gain Controller", "given a message after it failed on one");

        const std::string id = message->getMessageID() != nullptr ? message->getMessageID() : "";

        if (id == "throw" || id == "crash")
        {
            failed = true;

            if (id == "crash")
            {
                volatile int* volatile nowhere = nullptr;
                *nowhere = 1;
            }

            throw std::runtime_error("the controller's own exception, from a message");
        }

        return EditController::notify(message);
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
    {
        const auto result = EditController::setParamNormalized(id, value);

        if (componentHandler == nullptr)
            return result;

        if (id == testIds::editParameter)
        {
            componentHandler->beginEdit(testIds::gainParameter);
            componentHandler->performEdit(testIds::gainParameter, value);
            componentHandler->endEdit(testIds::gainParameter);
            componentHandler->performEdit(testIds::unlistedParameter, 0.5);
        }

        if (id == testIds::dirtyParameter)
            if (FUnknownPtr<IComponentHandler2> handler(componentHandler); handler)
                for (int i = 0; i < 100; ++i)
                    handler->setDirty(true);

        if (id == testIds::echoParameter)
        {
            ++echoes;
            componentHandler->performEdit(id, value);
        }

        if (id == testIds::roundedEchoParameter)
        {
            ++roundedEchoes;
            componentHandler->performEdit(id, static_cast<float>(value));
        }

        return result;
    }

    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 string) override
    {
        if (id != testIds::queryParameter)
            return EditController::getParamStringByValue(id, value, string);

        char text[128];
        std::snprintf(text, sizeof text, "gain=%.6f meter=%.6f echo=%d rounded=%d",
                      getParamNormalized(testIds::gainParameter), getParamNormalized(testIds::meterParameter),
                      echoes.load(), roundedEchoes.load());

        for (std::size_t i = 0; i < sizeof text; ++i)
            if ((string[i] = static_cast<TChar>(text[i])) == 0)
                break;

        return kResultOk;
    }

private:
    std::atomic<int> echoes{0}, roundedEchoes{0};
    bool failed = false;
};

// A plugin with a thousand parameters, all automatable, its processing the gain's.
class Many : public Checked
{
public:
    Many() : Checked("Many", FUID(SYMBOLENGINE_TEST_ID(0x0B))) {}
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Many); }
};

class ManyController : public EditController
{
public:
    static FUnknown* make(void*) { return static_cast<IEditController*>(new ManyController); }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = EditController::initialize(context);

        for (unsigned id = 1; result == kResultOk && id <= testIds::manyParameters; ++id)
            parameters.addParameter(STR16("Many"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate,
                                    static_cast<int32>(id));

        return result;
    }
};

// Gain, but passing over a call with no samples before it reads its changes, as plugins built on DPF do.
class SkipsEmpty : public Gain
{
public:
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new SkipsEmpty); }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (data.numSamples == 0)
            return kResultOk;

        return Gain::process(data);
    }
};

// Describes its buses only once an arrangement is set on them, as some plugins do: before, it says it can't.
class Arranged : public Checked
{
public:
    Arranged() : Checked("Arranged") {}
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Arranged); }

    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection direction, int32 index, BusInfo& info) override
    {
        if (type == kAudio && !arranged)
            return kResultFalse;

        return Checked::getBusInfo(type, direction, index, info);
    }

    tresult PLUGIN_API getBusArrangement(BusDirection direction, int32 index, SpeakerArrangement& arrangement) override
    {
        if (!arranged)
            return kResultFalse;

        return Checked::getBusArrangement(direction, index, arrangement);
    }
};

// Mono in and out, and nothing else: asked for stereo, it says no and stays mono.
class Mono : public Checked
{
public:
    Mono() : Checked("Mono") {}
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Mono); }

protected:
    void addBuses() override
    {
        addAudioInput(STR16("Input"), SpeakerArr::kMono);
        addAudioOutput(STR16("Output"), SpeakerArr::kMono);
    }

    tresult arrange(SpeakerArrangement* inputs, int32 ins, SpeakerArrangement* outputs, int32 outs) override
    {
        if ((ins > 0 && inputs[0] != SpeakerArr::kMono) || (outs > 0 && outputs[0] != SpeakerArr::kMono))
            return kResultFalse;

        return Checked::arrange(inputs, ins, outputs, outs);
    }
};

// Takes audio in and gives none out.
class NoOutput : public Checked
{
public:
    NoOutput() : Checked("No output") {}
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new NoOutput); }

protected:
    void addBuses() override { addAudioInput(STR16("Input"), SpeakerArr::kStereo); }
};

// One object with both parts that crashes, writing through a null pointer, or throws, when told to by a parameter, its
// audio passing through until then.
class Faulting : public SingleComponentEffect
{
public:
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Faulting); }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize(context);
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        parameters.addParameter(STR16("Fault in process"), nullptr, 1, 0.0, 0, testIds::faultInProcess);
        parameters.addParameter(STR16("Fault on main"), nullptr, 1, 0.0, 0, testIds::faultOnMain);
        parameters.addParameter(STR16("Throw in process"), nullptr, 1, 0.0, 0, testIds::throwInProcess);
        parameters.addParameter(STR16("Hang on main"), nullptr, 1, 0.0, 0, testIds::hangOnMain);
        parameters.addParameter(STR16("Hang in process"), nullptr, 1, 0.0, 0, testIds::hangInProcess);
        parameters.addParameter(STR16("Slow in process"), nullptr, 1, 0.0, 0, testIds::slowInProcess);
        return result;
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
    {
        if (id == testIds::faultOnMain && value > 0.5)
            crash();

        if (id == testIds::hangOnMain && value > 0.5)
            std::this_thread::sleep_for(std::chrono::minutes(1));

        return SingleComponentEffect::setParamNormalized(id, value);
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (data.inputParameterChanges != nullptr)
            for (int32 q = 0; q < data.inputParameterChanges->getParameterCount(); ++q)
                if (auto* changes = data.inputParameterChanges->getParameterData(q);
                    changes != nullptr && changes->getPointCount() > 0)
                {
                    int32 offset = 0;
                    ParamValue value = 0.0;
                    changes->getPoint(changes->getPointCount() - 1, offset, value);

                    if (value > 0.5 && changes->getParameterId() == testIds::faultInProcess)
                        crash();

                    if (value > 0.5 && changes->getParameterId() == testIds::throwInProcess)
                        throw std::runtime_error("told to throw while processing");

                    if (value > 0.5 && changes->getParameterId() == testIds::hangInProcess)
                        std::this_thread::sleep_for(std::chrono::minutes(1));

                    if (changes->getParameterId() == testIds::slowInProcess)
                        slow = value > 0.5;
                }

        if (slow && ++calls % 100 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const int32 channels = data.numOutputs > 0 && data.numInputs > 0 ? data.outputs[0].numChannels : 0;

        for (int32 c = 0; c < channels; ++c)
            for (int32 i = 0; i < data.numSamples; ++i)
                data.outputs[0].channelBuffers32[c][i] = 0.25f * data.inputs[0].channelBuffers32[c][i];

        return kResultOk;
    }

private:
    static void crash()
    {
        volatile int* volatile nowhere = nullptr;
        *nowhere = 1;
    }

    // Whether it sleeps in every hundredth process call, counted from the one it is told in.
    bool slow = false;
    int calls = 0;
};

// One object with both parts that makes the calls a real-time thread never should, as a parameter tells it, knowing
// nothing of what watches it, as another maker's plugin wouldn't.
class Unsafe : public SingleComponentEffect
{
public:
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Unsafe); }

    ~Unsafe() override
    {
        leaving = true;

        if (worker.joinable())
            worker.join();
    }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize(context);
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        parameters.addParameter(STR16("Does"), nullptr, 0, 0.0, 0, testIds::unsafeDoes);
        parameters.addParameter(STR16("At start"), nullptr, 0, 0.0, 0, testIds::unsafeAtStart);
        return result;
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
    {
        if (id == testIds::unsafeAtStart)
            atStart = testIds::unsafeStep(value);

        return SingleComponentEffect::setParamNormalized(id, value);
    }

    tresult PLUGIN_API setProcessing(TBool state) override
    {
        if (state)
            act(atStart);

        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (data.inputParameterChanges != nullptr)
            for (int32 q = 0; q < data.inputParameterChanges->getParameterCount(); ++q)
                if (auto* changes = data.inputParameterChanges->getParameterData(q);
                    changes != nullptr && changes->getPointCount() > 0 &&
                    changes->getParameterId() == testIds::unsafeDoes)
                {
                    int32 offset = 0;
                    ParamValue value = 0.0;
                    changes->getPoint(changes->getPointCount() - 1, offset, value);
                    does = testIds::unsafeStep(value);
                    calls = 0;
                }

        act(does);
        ++calls;
        const int32 channels = data.numOutputs > 0 && data.numInputs > 0 ? data.outputs[0].numChannels : 0;

        for (int32 c = 0; c < channels; ++c)
            for (int32 i = 0; i < data.numSamples; ++i)
                data.outputs[0].channelBuffers32[c][i] = data.inputs[0].channelBuffers32[c][i];

        return kResultOk;
    }

private:
    // A function for each number, so each is a place of its own.
    template <int place>
    SYMBOLENGINE_TEST_NOINLINE static void allocateAt()
    {
        allocate();
        keep = keep + place;
    }

    template <int... each>
    static void allocateAtEach(std::integer_sequence<int, each...>)
    {
        (allocateAt<each>(), ...);
    }

    SYMBOLENGINE_TEST_NOINLINE static void allocate()
    {
        void* volatile memory = std::malloc(64);
        std::free(memory);
    }

    void act(int what)
    {
        switch (what)
        {
            case testIds::doesMalloc: allocate(); break;
            case testIds::doesNew:
            {
                int* volatile memory = new int[64];
                delete[] memory;
                break;
            }
            case testIds::doesVector:
            {
                std::vector<int> values;

                for (int i = 0; i < 100; ++i)
                    values.push_back(i);

                keep = keep + values.back();
                break;
            }
            case testIds::doesString:
            {
                std::string text = "x";
                text.append(200, 'y');
                keep = keep + static_cast<int>(text.size());
                break;
            }
            case testIds::doesLock:
            {
                const std::lock_guard held(lock);
                keep = keep + 1;
                break;
            }
            case testIds::doesTryLock:
                if (lock.try_lock())
                    lock.unlock();
                break;
            case testIds::doesSleep: std::this_thread::sleep_for(std::chrono::microseconds(1)); break;
            case testIds::doesFile:
                if (auto* file = std::fopen(SYMBOLENGINE_TEST_NOWHERE, "r"))
                    std::fclose(file);
                break;
            case testIds::doesThread: std::thread([] {}).join(); break;
            case testIds::doesSystemAllocation: allocateThroughTheSystem(); break;
            case testIds::doesMallocOnce:
                if (calls == 0)
                    allocate();
                break;
            case testIds::doesMallocEveryTenth:
                if (calls % 10 == 0)
                    allocate();
                break;
            case testIds::doesManyPlaces:
                allocateAtEach(std::make_integer_sequence<int, 100>{});
                break;
            case testIds::doesWorkerAllocation:
                if (!worker.joinable())
                    worker = std::thread([this] {
                        while (!leaving)
                        {
                            if (asked.load() != answered.load())
                            {
                                allocate();
                                answered = asked.load();
                            }
                        }
                    });

                // Only spins, a spin here being the plugin's own affair: what the worker does is on a thread of its own.
                for (const auto wanted = ++asked; answered.load() != wanted;)
                {
                }
                break;
            case testIds::doesSystemLock: lockTheSystemsWay(); break;
            case testIds::doesMallocTwice:
                for (int i = 0; i < 2; ++i)
                    allocate();
                break;
            case testIds::doesFreeNothing:
            {
                void* volatile nothing = nullptr;
                std::free(nothing);
                break;
            }
            case testIds::doesReadWriteLock: writeLockTheSystemsWay(); break;
            case testIds::doesConditionWait:
            {
                std::unique_lock held(lock);
                condition.wait_for(held, std::chrono::microseconds(1));
                break;
            }
            default: break;
        }
    }

    // What this system's own libraries are built on, under the C library's and the C++ library's calls.
    static void allocateThroughTheSystem()
    {
#if defined(__APPLE__)
        const auto text = CFStringCreateWithCString(nullptr, "long enough to need memory of its own, this is",
                                                    kCFStringEncodingUTF8);
        CFRelease(text);
#elif defined(_WIN32)
        void* volatile memory = CoTaskMemAlloc(64);
        CoTaskMemFree(memory);
#else
        char* volatile copy = strdup("a copy");
        std::free(copy);
#endif
    }

    void lockTheSystemsWay()
    {
#if defined(__APPLE__)
        os_unfair_lock_lock(&systemLock);
        os_unfair_lock_unlock(&systemLock);
#elif defined(_WIN32)
        AcquireSRWLockExclusive(&systemLock);
        ReleaseSRWLockExclusive(&systemLock);
#else
        pthread_spin_lock(&systemLock);
        pthread_spin_unlock(&systemLock);
#endif
    }

    void writeLockTheSystemsWay()
    {
#if defined(_WIN32)
        AcquireSRWLockExclusive(&systemLock);
        ReleaseSRWLockExclusive(&systemLock);
#else
        pthread_rwlock_wrlock(&readWriteLock);
        pthread_rwlock_unlock(&readWriteLock);
#endif
    }

    inline static std::atomic<int> keep{0};
    int does = testIds::doesNothing;
    int calls = 0;
    std::atomic<int> atStart{testIds::doesNothing};
    std::mutex lock;
#if !defined(_WIN32)
    pthread_rwlock_t readWriteLock = PTHREAD_RWLOCK_INITIALIZER;
#endif
    std::condition_variable condition;
    std::thread worker;
    std::atomic<bool> leaving{false};
    std::atomic<int> asked{0}, answered{0};
#if defined(__APPLE__)
    os_unfair_lock systemLock = OS_UNFAIR_LOCK_INIT;
#elif defined(_WIN32)
    SRWLOCK systemLock = SRWLOCK_INIT;
#else
    pthread_spinlock_t systemLock = [] {
        pthread_spinlock_t made;
        pthread_spin_init(&made, PTHREAD_PROCESS_PRIVATE);
        return made;
    }();
#endif
};

// One object with both parts that asks for the context it needs, and keeps what it was given, which the text of the
// query parameter says.
class Timed : public SingleComponentEffect
{
public:
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Timed); }

    static constexpr uint32 needs = kNeedSystemTime | kNeedContinousTimeSamples | kNeedProjectTimeMusic |
                                    kNeedBarPositionMusic | kNeedTempo | kNeedTimeSignature | kNeedTransportState;

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize(context);
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        return result;
    }

    uint32 PLUGIN_API getProcessContextRequirements() override
    {
        asked = true;
        return needs;
    }

    tresult PLUGIN_API setActive(TBool state) override
    {
        if (state && !asked)
            misuse("Timed", "activated before it was asked what context it needs");

        return SingleComponentEffect::setActive(state);
    }

    tresult PLUGIN_API setProcessing(TBool state) override
    {
        if (!state)
            ++stops;

        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        checkContext("Timed", data, processSetup, clock);

        if (data.processContext != nullptr)
        {
            const auto& context = *data.processContext;
            const auto valid = [&context](uint32 flag) { return (context.state & flag) != 0; };

            if (!valid(ProcessContext::kTempoValid) || !valid(ProcessContext::kTimeSigValid) ||
                !valid(ProcessContext::kProjectTimeMusicValid) || !valid(ProcessContext::kBarPositionValid) ||
                !valid(ProcessContext::kSystemTimeValid) || !valid(ProcessContext::kContTimeValid))
                misuse("Timed", "not given all the context it asked for");

            position = context.projectTimeSamples;
            playing = valid(ProcessContext::kPlaying);
            tempo = context.tempo;
            numerator = context.timeSigNumerator;
            denominator = context.timeSigDenominator;
            music = context.projectTimeMusic;
            bar = context.barPositionMusic;
        }

        const int32 channels = data.numOutputs > 0 && data.numInputs > 0 ? data.outputs[0].numChannels : 0;

        for (int32 c = 0; c < channels; ++c)
            for (int32 i = 0; i < data.numSamples; ++i)
                data.outputs[0].channelBuffers32[c][i] = data.inputs[0].channelBuffers32[c][i];

        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 string) override
    {
        if (id != testIds::queryParameter)
            return SingleComponentEffect::getParamStringByValue(id, value, string);

        char text[128];
        std::snprintf(text, sizeof text,
                      "position=%lld playing=%d tempo=%.1f signature=%d/%d music=%.3f bar=%.3f stops=%d",
                      static_cast<long long>(position.load()), playing.load() ? 1 : 0, tempo.load(), numerator.load(),
                      denominator.load(), music.load(), bar.load(), stops.load());

        for (std::size_t i = 0; i < sizeof text; ++i)
            if ((string[i] = static_cast<TChar>(text[i])) == 0)
                break;

        return kResultOk;
    }

private:
    ContextClock clock;
    std::atomic<bool> asked{false}, playing{false};
    std::atomic<int64> position{-1};
    std::atomic<double> tempo{0.0}, music{0.0}, bar{0.0};
    std::atomic<int> numerator{0}, denominator{0}, stops{0};
};

// One object with both parts, whose latency is 0 until it is active, 256 once it is, and 512 after its twentieth
// block, which it says from its process call, on the audio thread, as some plugins do.
class Latency : public SingleComponentEffect
{
public:
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Latency); }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        if (++initializations > 1)
            misuse("Latency", "initialized twice, though it is one object");

        const auto result = SingleComponentEffect::initialize(context);
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        return result;
    }

    tresult PLUGIN_API setActive(TBool state) override
    {
        active = state;
        return SingleComponentEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() override { return !active ? 0u : blocks >= 20 ? 512u : 256u; }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (++blocks == 20 && componentHandler != nullptr)
            componentHandler->restartComponent(kLatencyChanged);

        const int32 channels = data.numOutputs > 0 && data.numInputs > 0 ? data.outputs[0].numChannels : 0;

        for (int32 c = 0; c < channels; ++c)
            for (int32 i = 0; i < data.numSamples; ++i)
                data.outputs[0].channelBuffers32[c][i] = data.inputs[0].channelBuffers32[c][i];

        return kResultOk;
    }

private:
    std::atomic<int> initializations{0}, blocks{0};
    std::atomic<bool> active{false};
};

// Throws from its state call, as plugins have.
class Throwing : public Checked
{
public:
    Throwing() : Checked("Throwing") {}
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Throwing); }

    tresult PLUGIN_API setState(IBStream*) override { throw std::runtime_error("the test plugin's own exception"); }
};

}

BEGIN_FACTORY_DEF("SymbolEngine", "https://symbolengine.dev", "")

DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x01), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Gain", Vst::kDistributable, "Fx", "1.0.0", kVstVersionString, Gain::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x02), PClassInfo::kManyInstances,
           kVstComponentControllerClass, "Gain Controller", 0, "", "1.0.0", kVstVersionString, GainController::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x03), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Arranged", 0, "Fx", "1.0.0", kVstVersionString, Arranged::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x04), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Mono", 0, "Fx", "1.0.0", kVstVersionString, Mono::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x05), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "No output", 0, "Fx", "1.0.0", kVstVersionString, NoOutput::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x06), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Latency", 0, "Fx", "1.0.0", kVstVersionString, Latency::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x0A), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Many", 0, "Fx", "1.0.0", kVstVersionString, Many::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x0B), PClassInfo::kManyInstances,
           kVstComponentControllerClass, "Many Controller", 0, "", "1.0.0", kVstVersionString, ManyController::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x0D), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Faulting", 0, "Fx", "1.0.0", kVstVersionString, Faulting::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x0C), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Timed", 0, "Fx", "1.0.0", kVstVersionString, Timed::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x09), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Skips empty", 0, "Fx", "1.0.0", kVstVersionString, SkipsEmpty::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x08), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Throwing", 0, "Fx", "1.0.0", kVstVersionString, Throwing::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x0E), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Instrument", 0, "Instrument|Synth", "2.1.0", kVstVersionString, Gain::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x10), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Editor", 0, "Fx", "1.0.0", kVstVersionString, makeTestEditor)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x11), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Unsafe", 0, "Fx", "1.0.0", kVstVersionString, Unsafe::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x12), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Unsafe twin", 0, "Fx", "1.0.0", kVstVersionString, Unsafe::make)

END_FACTORY
