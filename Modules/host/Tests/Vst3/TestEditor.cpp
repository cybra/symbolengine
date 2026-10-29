// The editor test plugin, written straight against Steinberg's SDK: one object with both parts, passing its input
// through, whose view checks every call the host makes into it as the SDK's documentation of a view's life says, and
// misbehaves as views other hosts have met do. What was wrong goes to the file SYMBOLENGINE_TEST_VST3_LOG names.

#include "TestEditor.h"
#include "TestIds.h"

// First, before anything that includes the controller's interface, as in the module's other source.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace Steinberg;
using namespace Steinberg::Vst;

// The SDK's interfaces have no virtual destructors, as COM's don't: the view is only ever deleted as itself.
#if defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

namespace
{

void misuse(const std::string& what)
{
    static std::mutex lock;
    const std::lock_guard held(lock);

    if (const char* path = std::getenv("SYMBOLENGINE_TEST_VST3_LOG"))
        if (FILE* log = std::fopen(path, "ab"))
        {
            std::fprintf(log, "Editor: %s\n", what.c_str());
            std::fclose(log);
        }
}

class View;

// What the plugin's views saw, kept by the plugin and shared with each view, so a view a host lets go of after its
// plugin still has it.
struct Record
{
    std::thread::id mainThread;
    std::atomic<int> mode{testIds::wellBehaved};
    std::atomic<View*> view{nullptr};
    std::atomic<int> made{0}, live{0}, attached{0}, removed{0}, sized{0}, constraints{0}, scales{0}, offMain{0};
    std::atomic<int> width{0}, height{0}, ownWidth{0}, ownHeight{0}, nudges{0}, scalePercent{100}, scaleFirst{0};
    std::atomic<bool> offscreenMoved{false};
};

class View final : public IPlugView, public IPlugViewContentScaleSupport
{
public:
    View(std::shared_ptr<Record> shared, int behavior) : record(std::move(shared)), mode(behavior)
    {
        ++record->made;
        ++record->live;
        record->view = this;
    }

    ~View()
    {
        if (native != nullptr)
            misuse("let go of while still in the host's view");

        View* self = this;
        record->view.compare_exchange_strong(self, nullptr);
        --record->live;
    }

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override
    {
        check("isPlatformTypeSupported");
        return type != nullptr && testEditor::supported(type) ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) override
    {
        check("attached");

        if (frame == nullptr)
            misuse("attached before it was given its frame");

        if (native != nullptr)
            misuse("attached twice");

        if (parent == nullptr || type == nullptr || !testEditor::supported(type))
            return kResultFalse;

        if (!testEditor::laidOut(parent))
            misuse("attached to a view that isn't in its window, is flipped or doesn't clip");

        if (!testEditor::sized(parent, width, height))
            misuse("attached to a view not yet sized to it");

        if (!testEditor::awareAsParent(parent))
            misuse("attached with the thread's DPI awareness unlike its window's");

        native = testEditor::attach(parent, width, height);

        if (native == nullptr)
            return kResultFalse;

        if (!testEditor::takesDrops(native))
            misuse("couldn't take dropped files: its thread isn't set up for OLE");

        host = parent;
        ++record->attached;
        return kResultTrue;
    }

    tresult PLUGIN_API removed() override
    {
        check("removed");

        if (native == nullptr)
        {
            misuse("removed without being attached");
            return kResultFalse;
        }

        if (!testEditor::stillWhole(host, native))
            misuse("removed after the host's view was hidden, changed or taken apart");

        if (!testEditor::awareAsParent(host))
            misuse("removed with the thread's DPI awareness unlike its window's");

        record->offscreenMoved = record->offscreenMoved || testEditor::offscreenMoved(native);
        keepOwnSize();
        record->nudges = testEditor::nudges(native);
        testEditor::detach(native);
        native = nullptr;
        host = nullptr;
        ++record->removed;
        return kResultTrue;
    }

    tresult PLUGIN_API onWheel(float) override
    {
        check("onWheel");
        return kResultFalse;
    }

    tresult PLUGIN_API onKeyDown(char16, int16, int16) override
    {
        check("onKeyDown");
        return kResultFalse;
    }

    tresult PLUGIN_API onKeyUp(char16, int16, int16) override
    {
        check("onKeyUp");
        return kResultFalse;
    }

    tresult PLUGIN_API getSize(ViewRect* size) override
    {
        check("getSize");
        ++sizesAsked;

        if (size == nullptr)
            return kInvalidArgument;

        *size = ViewRect(0, 0, width, height);
        return kResultTrue;
    }

    tresult PLUGIN_API onSize(ViewRect* size) override
    {
        check("onSize");

        if (native == nullptr)
            misuse("resized before it was attached");
        else if (!testEditor::awareAsParent(host))
            misuse("resized with the thread's DPI awareness unlike its window's");

        if (size == nullptr)
            return kInvalidArgument;

        width = size->getWidth();
        height = size->getHeight();
        ++record->sized;
        record->width = width;
        record->height = height;

        // A view that can't be resized leaves its own view's size to the host, as such plugins do.
        if (native != nullptr && mode != testIds::fixedSize && mode != testIds::fixedButAsks)
            testEditor::resize(native, width, height);

        // Asking again from inside its resize, as views other hosts have met do.
        ViewRect asked = *size;
        bool asks = mode == testIds::asksSameSize;

        if (mode == testIds::keepsAspect && height != width / 2)
        {
            asked.bottom = asked.top + width / 2;
            asks = true;
        }

        if (mode == testIds::keepsGrowing)
        {
            asked.right += 1;
            asks = true;
        }

        if (asks && frame != nullptr)
            frame->resizeView(this, &asked);

        return kResultTrue;
    }

    tresult PLUGIN_API onFocus(TBool) override
    {
        check("onFocus");
        return kResultTrue;
    }

    tresult PLUGIN_API setFrame(IPlugFrame* given) override
    {
        onMain("setFrame");

        if (cleared)
            misuse("given a frame after its frame was cleared");

        frame = given;
        cleared = given == nullptr;
        return kResultTrue;
    }

    tresult PLUGIN_API canResize() override
    {
        check("canResize");
        return mode == testIds::fixedSize || mode == testIds::fixedButAsks ? kResultFalse : kResultTrue;
    }

    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override
    {
        check("checkSizeConstraint");
        ++record->constraints;

        if (rect == nullptr)
            return kInvalidArgument;

        if (mode == testIds::fixedSize)
            *rect = ViewRect(0, 0, 999, 999);
        else if (mode == testIds::onePixelMore)
        {
            rect->right += 1;
            rect->bottom += 1;
        }
        else
        {
            rect->right = rect->left + std::max<int32>(200, rect->getWidth());
            rect->bottom = rect->top + std::max<int32>(100, rect->getHeight());
        }

        return kResultTrue;
    }

    // The scale of its screen: its size from then on its size at 1 times that, as a view sized in pixels answers.
    tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override
    {
        check("setContentScaleFactor");

        if (record->scales++ == 0)
            record->scaleFirst = sizesAsked == 0 ? 1 : 0;

        if (host != nullptr && !testEditor::awareAsParent(host))
            misuse("given its scale with the thread's DPI awareness unlike its window's");

        if (factor > 0.0f)
        {
            width = static_cast<int>(std::lround(width / scale * factor));
            height = static_cast<int>(std::lround(height / scale * factor));
            scale = factor;
            record->scalePercent = static_cast<int>(std::lround(factor * 100.0));
        }

        return kResultTrue;
    }

    // Asks its host to be resized, on the main thread or from a thread of its own.
    void ask(int askedWidth, int askedHeight, bool fromThread)
    {
        ViewRect asked(0, 0, askedWidth, askedHeight);

        if (frame == nullptr)
        {
            misuse("told to ask to be resized with no frame");
            return;
        }

        if (fromThread)
            std::thread([this, asked]() mutable { frame->resizeView(this, &asked); }).join();
        else
            frame->resizeView(this, &asked);
    }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IPlugView::iid))
            *object = static_cast<IPlugView*>(this);
        else if (FUnknownPrivate::iidEqual(wanted, IPlugViewContentScaleSupport::iid))
            *object = static_cast<IPlugViewContentScaleSupport*>(this);
        else
        {
            *object = nullptr;
            return kNoInterface;
        }

        addRef();
        return kResultOk;
    }

    uint32 PLUGIN_API addRef() override { return ++references; }

    uint32 PLUGIN_API release() override
    {
        const auto left = --references;

        if (left == 0)
            delete this;

        return left;
    }

private:
    void onMain(const char* call)
    {
        if (std::this_thread::get_id() != record->mainThread)
        {
            ++record->offMain;
            misuse(std::string(call) + " called off the main thread");
        }
    }

public:
    // The size of its own native view now, kept for the query.
    void keepOwnSize()
    {
        int ownWidth = 0, ownHeight = 0;

        if (native != nullptr)
            testEditor::ownSize(native, ownWidth, ownHeight);

        record->ownWidth = ownWidth;
        record->ownHeight = ownHeight;
    }

private:
    // Every call but its release and setFrame: on the main thread, and never once its frame is cleared.
    void check(const char* call)
    {
        onMain(call);

        if (cleared)
            misuse(std::string(call) + " called after its frame was cleared");
    }

    std::shared_ptr<Record> record;
    int mode;
    std::atomic<uint32> references{1};
    IPlugFrame* frame = nullptr;
    bool cleared = false;
    void* host = nullptr;
    void* native = nullptr;
    int width = 600, height = 400, sizesAsked = 0;
    double scale = 1.0;
};

class Editor final : public SingleComponentEffect
{
public:
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize(context);
        record->mainThread = std::this_thread::get_id();
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        parameters.addParameter(STR16("View mode"), nullptr, testIds::editorModes - 1, 0.0, 0, testIds::viewMode);
        parameters.addParameter(STR16("Ask width"), nullptr, 0, 0.0, 0, testIds::askWidth);
        parameters.addParameter(STR16("Ask height"), nullptr, 0, 0.0, 0, testIds::askHeight);
        parameters.addParameter(STR16("Ask resize"), nullptr, 2, 0.0, 0, testIds::askResize);
        parameters.addParameter(STR16("Request editor"), nullptr, 1, 0.0, 0, testIds::requestEditor);
        return result;
    }

    tresult PLUGIN_API terminate() override
    {
        if (record->attached > record->removed)
            misuse("ended with its view still in the host's");

        if (record->live > 0)
            misuse("ended before its view was let go of");

        return SingleComponentEffect::terminate();
    }

    IPlugView* PLUGIN_API createView(FIDString name) override
    {
        if (name == nullptr || std::strcmp(name, ViewType::kEditor) != 0)
            return nullptr;

        if (std::this_thread::get_id() != record->mainThread)
            misuse("asked for a view off the main thread");

        if (record->live > 0)
            misuse("asked for a second view while one was live");

        return new View(record, record->mode);
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
    {
        const auto result = SingleComponentEffect::setParamNormalized(id, value);

        if (id == testIds::viewMode)
            record->mode = static_cast<int>(std::lround(value * (testIds::editorModes - 1)));

        if (id == testIds::askResize && value > 0.25)
        {
            if (auto* open = record->view.load())
                open->ask(static_cast<int>(std::lround(getParamNormalized(testIds::askWidth) * testIds::editorPixels)),
                          static_cast<int>(std::lround(getParamNormalized(testIds::askHeight) * testIds::editorPixels)),
                          value > 0.75);
            else
                misuse("told to ask to be resized with no view");
        }

        if (id == testIds::requestEditor && value > 0.5)
            if (FUnknownPtr<IComponentHandler2> handler(componentHandler); handler)
                handler->requestOpenEditor(ViewType::kEditor);

        return result;
    }

    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 string) override
    {
        if (id == testIds::scaleQuery)
        {
            char text[64];
            std::snprintf(text, sizeof text, "scale=%d first=%d", record->scalePercent.load(),
                          record->scaleFirst.load());
            return copy(text, string);
        }

        if (id != testIds::queryParameter)
            return SingleComponentEffect::getParamStringByValue(id, value, string);

        if (auto* open = record->view.load())
            open->keepOwnSize();

        char text[200];
        std::snprintf(text, sizeof text,
                      "made=%d live=%d attached=%d removed=%d sized=%d size=%dx%d own=%dx%d constraints=%d "
                      "scales=%d offMain=%d nudges=%d offscreenMoved=%d",
                      record->made.load(), record->live.load(), record->attached.load(), record->removed.load(),
                      record->sized.load(), record->width.load(), record->height.load(), record->ownWidth.load(),
                      record->ownHeight.load(), record->constraints.load(), record->scales.load(),
                      record->offMain.load(), record->nudges.load(), record->offscreenMoved.load() ? 1 : 0);

        return copy(text, string);
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        const int32 channels = data.numOutputs > 0 && data.numInputs > 0 ? data.outputs[0].numChannels : 0;

        for (int32 c = 0; c < channels; ++c)
            for (int32 i = 0; i < data.numSamples; ++i)
                data.outputs[0].channelBuffers32[c][i] = data.inputs[0].channelBuffers32[c][i];

        return kResultOk;
    }

private:
    static tresult copy(const char* text, String128 string)
    {
        for (std::size_t i = 0; i < 128; ++i)
            if ((string[i] = static_cast<TChar>(text[i])) == 0 || i == 127)
            {
                string[i] = 0;
                break;
            }

        return kResultOk;
    }

    std::shared_ptr<Record> record = std::make_shared<Record>();
};

}

#if defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif

FUnknown* makeTestEditor(void*)
{
    return static_cast<IAudioProcessor*>(new Editor);
}
