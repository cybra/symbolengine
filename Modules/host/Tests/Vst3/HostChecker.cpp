// Steinberg's host checker, which logs every mistake a host makes in its calls: its processor as Steinberg wrote it,
// and a controller of this module's own in place of its own, whose editor needs VSTGUI, writing what the processor logs
// to the file SYMBOLENGINE_HOST_CHECKER_LOG names, a line for each: its severity, its number, how many times and its
// words. What it finds outside its processing comes in messages, and what it finds in a block as values it reports, a
// bit for each thing it found, so a host that loses those is found out too.

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include "cids.h"
#include "hostcheckercontroller.h"
#include "hostcheckerprocessor.h"
#include "logevents.h"

#include <cstdlib>
#include <fstream>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{

void write(int64 id, int64 count)
{
    const char* const path = std::getenv("SYMBOLENGINE_HOST_CHECKER_LOG");
    if (path == nullptr || id < 0 || id >= kNumLogEvents)
        return;
    std::ofstream(path, std::ios::app) << logEventSeverity[id] << '|' << id << '|' << count << '|'
                                       << logEventDescriptions[id] << '\n';
}

class LogController : public EditController
{
public:
    static FUnknown* createInstance(void*) { return static_cast<IEditController*>(new LogController); }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = EditController::initialize(context);
        if (result != kResultOk)
            return result;

        parameters.addParameter(STR16("Load"), nullptr, 0, 0, ParameterInfo::kCanAutomate, kProcessingLoadTag);
        parameters.addParameter(STR16("Peaks"), nullptr, 0, 0, ParameterInfo::kCanAutomate, kGeneratePeaksTag);
        parameters.addParameter(STR16("Latency"), nullptr, 0, 0, ParameterInfo::kCanAutomate, kLatencyTag);
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                                kBypassTag);
        parameters.addParameter(STR16("Mode"), nullptr, 2, 0, ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden,
                                kParamProcessModeTag);

        for (uint32 i = 0; i < HostChecker::kParamWarnCount; ++i)
            parameters.addParameter(STR16("Found"), nullptr, HostChecker::kParamWarnStepCount, 0,
                                    ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden, kProcessWarnTag + i);
        return kResultOk;
    }

    tresult PLUGIN_API connect(IConnectionPoint* other) override
    {
        const auto result = EditController::connect(other);

        // The processor checks the changes it is given against the parameters it is told of.
        for (const auto id : {kProcessingLoadTag, kGeneratePeaksTag, kLatencyTag, kBypassTag})
            if (auto message = owned(allocateMessage()))
            {
                message->setMessageID("Parameter");
                message->getAttributes()->setInt("ID", id);
                sendMessage(message);
            }

        return result;
    }

    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) override
    {
        if (tag >= kProcessWarnTag && tag < kProcessWarnTag + HostChecker::kParamWarnCount)
        {
            const auto found = static_cast<uint32>(value * HostChecker::kParamWarnStepCount + 0.5);
            for (uint32 bit = 0; bit < HostChecker::kParamWarnBitCount; ++bit)
                if ((found & (uint32(1) << bit)) != 0)
                    write((tag - kProcessWarnTag) * HostChecker::kParamWarnBitCount + bit, 1);
        }
        return EditController::setParamNormalized(tag, value);
    }

    tresult PLUGIN_API notify(IMessage* message) override
    {
        if (message != nullptr && FIDStringsEqual(message->getMessageID(), "LogEvent"))
        {
            int64 id = -1, count = 0;
            message->getAttributes()->getInt("ID", id);
            message->getAttributes()->getInt("Count", count);
            write(id, count);
        }
        return kResultOk;
    }
};

}

BEGIN_FACTORY_DEF("SymbolEngine Tests", "", "")

DEF_CLASS2(INLINE_UID_FROM_FUID(HostCheckerProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Host Checker", Vst::kDistributable, "Fx", "1.0.0", kVstVersionString, HostCheckerProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(HostCheckerControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "Host Checker", 0, "", "1.0.0", kVstVersionString, LogController::createInstance)

END_FACTORY
