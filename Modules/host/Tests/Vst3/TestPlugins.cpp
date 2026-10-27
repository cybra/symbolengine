// The test module's plugins, written straight against Steinberg's SDK. Each checks the calls a host makes into it, as
// Steinberg's host checker does, and writes what was wrong to the file SYMBOLENGINE_TEST_VST3_LOG names; most also
// misbehave one way, as plugins do.

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
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

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

// What a processor checks of its host: every call but process and setProcessing on the thread it was started on, in
// the order the SDK's call sequence allows, and every block's buffers as its buses say.
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

        for (int32 i = 0; i < data.numOutputs; ++i)
            check(data.outputs[i], audioOutputs[static_cast<std::size_t>(i)]);

        // The gain heard from the host's changes, the last point of its queue, whatever the block's length.
        if (data.inputParameterChanges != nullptr)
            for (int32 q = 0; q < data.inputParameterChanges->getParameterCount(); ++q)
                if (auto* changes = data.inputParameterChanges->getParameterData(q);
                    changes != nullptr && changes->getParameterId() == testIds::gainParameter &&
                    changes->getPointCount() > 0)
                {
                    int32 offset = 0;
                    ParamValue value = 0.0;
                    changes->getPoint(changes->getPointCount() - 1, offset, value);
                    gain = static_cast<float>(value);
                }

        if (data.numSamples > 0 && data.numInputs > 0 && data.numOutputs > 0)
            play(data);

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

    // The input at its gain, half to begin with, channel for channel, on the main buses.
    virtual void play(ProcessData& data)
    {
        const auto& in = data.inputs[0];
        auto& out = data.outputs[0];

        for (int32 c = 0; c < out.numChannels; ++c)
            for (int32 i = 0; i < data.numSamples; ++i)
                out.channelBuffers32[c][i] =
                    in.numChannels > 0 ? gain * in.channelBuffers32[std::min(c, in.numChannels - 1)][i] : 0.0f;
    }

    std::atomic<float> gain{0.5f};

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
        return kResultOk;
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
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x09), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Skips empty", 0, "Fx", "1.0.0", kVstVersionString, SkipsEmpty::make)
DEF_CLASS2(INLINE_UID(0x53796D62, 0x54657374, 0x56535433, 0x08), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Throwing", 0, "Fx", "1.0.0", kVstVersionString, Throwing::make)

END_FACTORY
