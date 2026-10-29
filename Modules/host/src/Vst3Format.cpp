#include "symbolengine/host/Vst3Format.h"

#include "Vst3Module.h"
#include "Vst3View.h"

#include "PluginFiles.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/core/Text.h>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace symbolengine
{

using namespace Steinberg;
using namespace Steinberg::Vst;

// The SDK's interfaces have no virtual destructors, as COM's don't: the objects here are only ever deleted as
// themselves, by their own release.
#if defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

namespace
{

// What the plugin is told the program is called. The SDK's class counts no references, being meant to live for the
// whole program; this one counts them, so it goes once nothing holds it.
class HostContext final : public HostApplication
{
public:
    explicit HostContext(std::string name) : programName(std::move(name)) {}

    tresult PLUGIN_API getName(String128 name) override
    {
        return StringConvert::convert(programName, name) ? kResultTrue : kInternalError;
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
    std::string programName;
    std::atomic<uint32> references{1};
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

    tresult PLUGIN_API beginEdit(ParamID id) override { return edit({PluginEdit::Kind::begin, id, 0.0}); }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override
    {
        return edit({PluginEdit::Kind::value, id, value});
    }
    tresult PLUGIN_API endEdit(ParamID id) override { return edit({PluginEdit::Kind::end, id, 0.0}); }

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

    // Its editor, asked for: told to the program, which opens it or brings it in front.
    tresult PLUGIN_API requestOpenEditor(FIDString name) override
    {
        auto* const listener = client.load();

        if (listener == nullptr || (name != nullptr && std::strcmp(name, ViewType::kEditor) != 0))
            return kResultFalse;

        listener->notice(PluginNotice::editor);
        return kResultTrue;
    }

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

    tresult edit(const PluginEdit& edit) const
    {
        auto* const listener = client.load();

        if (listener == nullptr)
            return kResultFalse;

        listener->edit(edit);
        return kResultOk;
    }
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
// One parameter's changes in a call, in order of their frames. The host owns it and makes its room when the plugin is
// activated, so neither the host filling it nor the plugin writing to it allocates on the audio thread.
class ChangeQueue final : public IParamValueQueue
{
public:
    struct Point
    {
        int32 offset = 0;
        ParamValue value = 0.0;
    };

    ParamID id = kNoParamId;
    Point* points = nullptr;
    int32 count = 0, room = 0;

    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }

    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override
    {
        if (index < 0 || index >= count)
            return kResultFalse;

        offset = points[index].offset;
        value = points[index].value;
        return kResultTrue;
    }

    // A point at its frame, in order, replacing one at the same frame; with no room left the last is replaced, so the
    // last value is always the one given last.
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32& index) override
    {
        if (room <= 0)
            return kResultFalse;

        int32 at = count;

        while (at > 0 && points[at - 1].offset > offset)
            --at;

        if (at > 0 && points[at - 1].offset == offset)
        {
            points[at - 1].value = value;
            index = at - 1;
            return kResultTrue;
        }

        if (count == room)
        {
            points[count - 1] = {offset, value};
            index = count - 1;
            return kResultTrue;
        }

        std::copy_backward(points + at, points + count, points + count + 1);
        points[at] = {offset, value};
        ++count;
        index = at;
        return kResultTrue;
    }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(wanted, IParamValueQueue::iid))
        {
            *object = static_cast<IParamValueQueue*>(this);
            return kResultOk;
        }

        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

// A call's changes, a queue for each parameter changed: the host's for the processor, filled from the block's changes,
// or the processor's own for the host, which it writes.
class ChangeList final : public IParameterChanges
{
public:
    // Room for this many parameters at once, made on the main thread: the host's points all in one pool, each of the
    // processor's queues with a few of its own.
    void make(int32 parameters, bool written)
    {
        queues.assign(static_cast<std::size_t>(std::max(64, parameters + 16)), ChangeQueue{});
        points.assign(written ? queues.size() * pointsWritten : static_cast<std::size_t>(maximumBlockChanges), {});
        order.assign(written ? 0 : static_cast<std::size_t>(maximumBlockChanges), 0);

        if (written)
            for (std::size_t q = 0; q < queues.size(); ++q)
            {
                queues[q].points = points.data() + q * pointsWritten;
                queues[q].room = pointsWritten;
            }

        used = 0;
    }

    void clear() noexcept
    {
        for (int32 q = 0; q < used; ++q)
            queues[static_cast<std::size_t>(q)].count = 0;

        used = 0;
    }

    // The block's changes, grouped by parameter in order of their frames, each within the call's frames; on the
    // audio thread, sorting in place.
    void fill(const PluginParameterChange* changes, int count, int frames) noexcept
    {
        used = 0;
        const auto given = std::min(std::max(0, count), static_cast<int>(order.size()));
        const int last = std::max(1, frames) - 1;

        for (int i = 0; i < given; ++i)
            order[static_cast<std::size_t>(i)] = i;

        std::sort(order.begin(), order.begin() + given, [changes](int a, int b) {
            if (changes[a].id != changes[b].id)
                return changes[a].id < changes[b].id;

            return changes[a].offset != changes[b].offset ? changes[a].offset < changes[b].offset : a < b;
        });

        std::size_t at = 0;

        for (int k = 0; k < given && static_cast<std::size_t>(used) < queues.size();)
        {
            auto& queue = queues[static_cast<std::size_t>(used++)];
            queue.id = changes[order[static_cast<std::size_t>(k)]].id;
            queue.points = points.data() + at;
            queue.count = 0;

            for (; k < given && changes[order[static_cast<std::size_t>(k)]].id == queue.id; ++k)
            {
                const auto& change = changes[order[static_cast<std::size_t>(k)]];
                const auto offset = static_cast<int32>(std::clamp(change.offset, 0, last));

                if (queue.count > 0 && queue.points[queue.count - 1].offset == offset)
                    queue.points[queue.count - 1].value = change.value;
                else if (at < points.size())
                {
                    queue.points[queue.count++] = {offset, change.value};
                    ++at;
                }
            }

            queue.room = queue.count;
        }
    }

    int32 PLUGIN_API getParameterCount() override { return used; }

    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        return index >= 0 && index < used ? &queues[static_cast<std::size_t>(index)] : nullptr;
    }

    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override
    {
        for (int32 q = 0; q < used; ++q)
            if (queues[static_cast<std::size_t>(q)].id == id)
            {
                index = q;
                return &queues[static_cast<std::size_t>(q)];
            }

        if (static_cast<std::size_t>(used) >= queues.size())
            return nullptr;

        auto& queue = queues[static_cast<std::size_t>(used)];
        queue.id = id;
        queue.count = 0;
        index = used++;
        return &queue;
    }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(wanted, IParameterChanges::iid))
        {
            *object = static_cast<IParameterChanges*>(this);
            return kResultOk;
        }

        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

private:
    static constexpr std::size_t pointsWritten = 4;
    std::vector<ChangeQueue> queues;
    std::vector<ChangeQueue::Point> points;
    std::vector<int> order;
    int32 used = 0;
};

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

#if defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif

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

// The system's views a plugin's editor is put in.
#if defined(__APPLE__)
const FIDString platformType = kPlatformTypeNSView;
#elif defined(_WIN32)
const FIDString platformType = kPlatformTypeHWND;
#else
const FIDString platformType = nullptr;
#endif

// Whether its editors are sized in pixels, and given the scale of the screen they are on: on Windows. On a Mac they are
// sized in points, and a view given a scale there draws at the wrong size.
#if defined(_WIN32)
constexpr bool givenScale = true;
#else
constexpr bool givenScale = false;
#endif

// The smallest size an editor is given, raising one that asks for less.
constexpr int smallestWidth = 100, smallestHeight = 50;

class Vst3Editor;

#if defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

// What a plugin's view asks of the host: to be resized, which is done on the main thread, one asked from another thread
// handed over to it, and nothing once the editor is closing.
class PlugFrame final : public IPlugFrame, Counted
{
public:
    PlugFrame(Vst3Editor* owner, std::shared_ptr<Vst3Format::Shared> format) : editor(owner), shared(std::move(format))
    {
    }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override;

    // On the main thread, as the editor closes.
    void undo() { editor = nullptr; }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override
    {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IPlugFrame::iid))
        {
            *object = static_cast<IPlugFrame*>(this);
            addRef();
            return kResultOk;
        }

        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return addOne(); }
    uint32 PLUGIN_API release() override { return releaseOne(this); }

private:
    Vst3Editor* editor;
    std::shared_ptr<Vst3Format::Shared> shared;
};

#if defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif

// A plugin's view in the program's, opened and closed in the order Steinberg's documentation of a view's life gives:
// made, its kind of view checked, given its frame, on Windows given its screen's scale, sized and the program's view
// sized to it, put in it, given the scale again; and taken out while the program's view is whole, its frame cleared,
// let go of. Every call into it is made with the thread's DPI awareness the program's view's. A resize it asks for
// resizes the program's view and is told to it once, in the same call; one it asks for again from inside being told,
// as some views do, is the next size told, up to a few in one call, so it never loops.
class Vst3Editor final : public PluginFormatEditor
{
public:
    Vst3Editor(IPtr<IPlugView> made, void* parentView, PluginEditorFrame& holder,
               std::shared_ptr<Vst3Format::Shared> format)
        : view(std::move(made)), parent(parentView), frame(holder), shared(std::move(format))
    {
    }

    ~Vst3Editor() override
    {
        const vst3View::ParentAwareness awareness(parent);

        if (plugFrame)
            plugFrame->undo();

        if (attached)
            view->removed();

        if (framed)
            view->setFrame(nullptr);

        view = nullptr;
    }

    bool open(PluginError& error)
    {
        const vst3View::ParentAwareness awareness(parent);
        plugFrame = owned(new PlugFrame(this, shared));
        view->setFrame(plugFrame);
        framed = true;

        // Its screen's scale before its size is first asked for, so it answers with the size for that scale.
        if (givenScale)
            scaled = FUnknownPtr<IPlugViewContentScaleSupport>(view);

        if (scaled)
            scaled->setContentScaleFactor(static_cast<IPlugViewContentScaleSupport::ScaleFactor>(frame.scale()));

        ViewRect rect{};
        const auto own = view->getSize(&rect) == kResultTrue ? PluginEditorSize{rect.getWidth(), rect.getHeight()}
                                                             : PluginEditorSize{};
        current = raised(own);
        frame.place(current);

        if (view->attached(parent, platformType) != kResultTrue)
        {
            error = {"Its editor couldn't be put in its window.", "attached", 0};
            return false;
        }

        attached = true;

        // Again, now it is on the screen the window is really on.
        if (scaled)
            scaled->setContentScaleFactor(static_cast<IPlugViewContentScaleSupport::ScaleFactor>(frame.scale()));

        canResize = view->canResize() == kResultTrue;

        // Some views say their real size only once they are in a window.
        if (view->getSize(&rect) == kResultTrue)
            if (const auto now = raised({rect.getWidth(), rect.getHeight()}); now != current)
            {
                current = now;
                frame.place(current);
            }

        vst3View::sizeOwnView(parent, current.width, current.height);
        return true;
    }

    PluginEditorSize size() override { return current; }

    bool resizable() override { return canResize; }

    PluginEditorSize constrain(PluginEditorSize dragged) override
    {
        const vst3View::ParentAwareness awareness(parent);
        ViewRect rect(0, 0, dragged.width, dragged.height);

        if (view->checkSizeConstraint(&rect) != kResultTrue)
            return raised(dragged);

        return raised({rect.getWidth(), rect.getHeight()});
    }

    void resized(PluginEditorSize size) override
    {
        const vst3View::ParentAwareness awareness(parent);

        if (size != current)
            tell(size, false);
    }

    // The scale of its new screen, and the size it says it takes there.
    void rescaled(double scale) override
    {
        if (!scaled)
            return;

        const vst3View::ParentAwareness awareness(parent);
        scaled->setContentScaleFactor(static_cast<IPlugViewContentScaleSupport::ScaleFactor>(scale));

        if (ViewRect rect{}; view->getSize(&rect) == kResultTrue)
            if (const auto size = raised({rect.getWidth(), rect.getHeight()}); size != current)
                tell(size, true);
    }

    // A resize the view asked for, on the main thread.
    tresult asked(IPlugView* from, const ViewRect& rect)
    {
        if (from != view.get())
            return kInvalidArgument;

        const vst3View::ParentAwareness awareness(parent);

        const auto size = raised({rect.getWidth(), rect.getHeight()});

        if (telling)
        {
            if (size != told)
                wanted = size;

            return kResultTrue;
        }

        if (size != current)
            tell(size, true);

        return kResultTrue;
    }

private:
    // A size raised to the smallest, or to the view's own size where that is larger.
    PluginEditorSize raised(PluginEditorSize size)
    {
        if (size.width >= smallestWidth && size.height >= smallestHeight)
            return size;

        if (ViewRect own{};
            view->getSize(&own) == kResultTrue && own.getWidth() >= smallestWidth && own.getHeight() >= smallestHeight)
            return {own.getWidth(), own.getHeight()};

        return {std::max(size.width, smallestWidth), std::max(size.height, smallestHeight)};
    }

    // The program's view resized, if it isn't already, and the size told to the view, then each size it asks for
    // while being told, at most three in all.
    void tell(PluginEditorSize size, bool place)
    {
        for (int round = 0; round < 3; ++round)
        {
            current = size;

            if (place)
                frame.place(size);

            vst3View::sizeOwnView(parent, size.width, size.height);
            ViewRect rect(0, 0, size.width, size.height);
            telling = true;
            told = size;
            wanted.reset();
            view->onSize(&rect);
            telling = false;

            if (!wanted.has_value() || *wanted == size)
                return;

            size = *wanted;
            place = true;
        }
    }

    IPtr<IPlugView> view;
    void* parent;
    PluginEditorFrame& frame;
    std::shared_ptr<Vst3Format::Shared> shared;
    IPtr<PlugFrame> plugFrame;
    FUnknownPtr<IPlugViewContentScaleSupport> scaled;
    bool framed = false, attached = false, canResize = false, telling = false;
    PluginEditorSize current{}, told{};
    std::optional<PluginEditorSize> wanted;
};

tresult PLUGIN_API PlugFrame::resizeView(IPlugView* view, ViewRect* newSize)
{
    if (view == nullptr || newSize == nullptr)
        return kInvalidArgument;

    if (shared->options.onMainThread())
        return editor != nullptr ? editor->asked(view, *newSize) : kResultFalse;

    // Asked from another thread: done on the main thread, if the editor is still open then.
    IPtr<PlugFrame> self(this);
    IPtr<IPlugView> from(view);
    const auto rect = *newSize;
    shared->options.post([self, from, rect] {
        if (self->editor != nullptr)
            self->editor->asked(from, rect);
    });
    return kResultTrue;
}

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
        scratchMain.assign(silence.size(), 0.0f);

        // Queues for every parameter made now, so changes added on the audio thread allocate nothing.
        const auto parameterCount = controller ? controller->getParameterCount() : 0;
        inputChanges.make(parameterCount, false);
        outputChanges.make(parameterCount, true);

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

        // The count of processed samples and the system time run on from any earlier activation, never back.
        const auto continuous = context.continousTimeSamples;
        const auto systemTime = context.systemTime;
        context = {};
        context.continousTimeSamples = continuous;
        context.systemTime = systemTime;
        context.sampleRate = setup.rate;

        // What context it needs, asked once, before it is first activated; a plugin that can't say is given all the
        // host has.
        if (!asked.has_value())
        {
            FUnknownPtr<IProcessContextRequirements> requirements(processor);
            asked = requirements ? requirements->getProcessContextRequirements() : ~uint32{0};
        }

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

    void startProcessing() override { processor->setProcessing(true); }

    void stopProcessing() override { processor->setProcessing(false); }

    bool process(PluginBlock& block) override
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

        inputChanges.fill(block.changes, block.changeCount, block.frames);
        lastTime = block.time != nullptr ? *block.time : PluginTime{};
        const bool fine = run(block.frames, lastTime);
        lastTime.position += block.frames;

        // The last value the processor wrote of each parameter.
        block.reportedCount = 0;

        for (int32 q = 0; q < outputChanges.getParameterCount() && block.reportedCount < block.reportedRoom; ++q)
        {
            auto* const written = outputChanges.getParameterData(q);
            int32 offset = 0;
            ParamValue value = 0.0;

            if (written != nullptr && written->getPointCount() > 0 &&
                written->getPoint(written->getPointCount() - 1, offset, value) == kResultTrue)
                block.reported[block.reportedCount++] = {written->getParameterId(), offset, value};
        }

        return fine;
    }

    // Values handed over with no audio: a call with no samples and every bus's buffers, never null ones. A plugin found
    // to pass over such a call without reading its changes, as some do, is given a short block of silence instead.
    void flush(const PluginParameterChange* changes, int count) override
    {
        if (!passesOverEmpty)
        {
            const auto before = count > 0 ? componentState() : std::string();
            inputChanges.fill(changes, count, 0);
            run(0, idleTime());

            if (count == 0 || componentState() != before)
                return;

            passesOverEmpty = true;
        }

        inputChanges.fill(changes, count, 0);
        runSilence();
    }

    std::vector<PluginParameter> parameters() override
    {
        std::vector<PluginParameter> list;

        if (!controller)
            return list;

        // Counted once: some plugins take a long time over it.
        const auto count = controller->getParameterCount();

        for (int32 i = 0; i < count; ++i)
        {
            ParameterInfo info{};

            if (controller->getParameterInfo(i, info) != kResultOk)
                continue;

            PluginParameter parameter;
            parameter.id = info.id;
            parameter.name = StringConvert::convert(info.title);
            parameter.units = StringConvert::convert(info.units);
            parameter.defaultValue = info.defaultNormalizedValue;
            parameter.steps = std::max(0, info.stepCount);
            parameter.automatable = (info.flags & ParameterInfo::kCanAutomate) != 0;
            parameter.readOnly = (info.flags & ParameterInfo::kIsReadOnly) != 0;
            parameter.hidden = (info.flags & ParameterInfo::kIsHidden) != 0;
            parameter.bypass = (info.flags & ParameterInfo::kIsBypass) != 0;
            parameter.programChange = (info.flags & ParameterInfo::kIsProgramChange) != 0;
            list.push_back(std::move(parameter));
        }

        return list;
    }

    double parameterValue(std::uint32_t id) override { return controller ? controller->getParamNormalized(id) : 0.0; }

    void showParameterValue(std::uint32_t id, double value) override
    {
        if (controller)
            controller->setParamNormalized(id, value);
    }

    std::string parameterText(std::uint32_t id, double value) override
    {
        String128 text{};

        if (!controller || controller->getParamStringByValue(id, value, text) != kResultOk)
            return {};

        return StringConvert::convert(text);
    }

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

    bool hasEditor() override
    {
        if (platformType == nullptr || !controller)
            return false;

        // Asked of a view made and let go of without being put anywhere.
        const auto probe = owned(controller->createView(ViewType::kEditor));
        return probe && probe->isPlatformTypeSupported(platformType) == kResultTrue;
    }

    std::unique_ptr<PluginFormatEditor> openEditor(void* parent, PluginEditorFrame& frame, PluginError& error) override
    {
        if (platformType == nullptr)
        {
            error = {"Plugins' editors aren't shown on this system yet.", "openEditor", 0};
            return nullptr;
        }

        auto view = controller ? owned(controller->createView(ViewType::kEditor)) : IPtr<IPlugView>();

        if (!view)
        {
            error = {"It has no editor.", "createView", 0};
            return nullptr;
        }

        if (view->isPlatformTypeSupported(platformType) != kResultTrue)
        {
            error = {"Its editor can't be shown on this system.", "isPlatformTypeSupported", 0};
            return nullptr;
        }

        auto editor = std::make_unique<Vst3Editor>(std::move(view), parent, frame, shared);

        if (!editor->open(error))
            return nullptr;

        return editor;
    }

private:
    std::string componentState()
    {
        IPtr<HostStream> stream = owned(new HostStream);
        return component->getState(stream) == kResultOk ? stream->bytes : std::string();
    }

    // A short block of silence, processing started and stopped around it, its output thrown away.
    void runSilence()
    {
        const int frames = std::clamp(processSetup.maxSamplesPerBlock, 1, 64);

        for (const auto direction : {kInput, kOutput})
            if (main[direction] >= 0)
                for (auto& pointer : channelPointers[direction][static_cast<std::size_t>(main[direction])])
                    pointer = direction == kInput ? silence.data() : scratchMain.data();

        processor->setProcessing(true);
        run(frames, idleTime());
        processor->setProcessing(false);
        std::fill(silence.begin(), silence.end(), 0.0f);
    }

    // Where the program was when nothing plays the plugin: where it last played it, stopped.
    PluginTime idleTime() const noexcept
    {
        auto idle = lastTime;
        idle.playing = idle.recording = idle.discontinuity = false;
        idle.systemNanoseconds = 0;
        return idle;
    }

    // The context for a call: the set-up's rate, the program's position and transport, the system time rising, and
    // its tempo and meter, or, where it has none, 120 beats a minute in 4/4 for a plugin that asks for them or for
    // positions in music.
    void fillContext(const PluginTime& time) noexcept
    {
        const auto wanted = asked.value_or(~uint32{0});
        context.state = ProcessContext::kContTimeValid | ProcessContext::kSystemTimeValid;
        context.projectTimeSamples = time.position;

        const auto now = time.systemNanoseconds != 0 ? time.systemNanoseconds
                                                     : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                           std::chrono::steady_clock::now().time_since_epoch())
                                                           .count();
        context.systemTime = std::max<int64>(now, context.systemTime + 1);

        if (time.playing)
            context.state |= ProcessContext::kPlaying;

        if (time.recording)
            context.state |= ProcessContext::kRecording;

        const bool wantsTempo =
            (wanted & (IProcessContextRequirements::kNeedTempo | IProcessContextRequirements::kNeedProjectTimeMusic |
                       IProcessContextRequirements::kNeedBarPositionMusic)) != 0;
        const bool wantsMeter = (wanted & (IProcessContextRequirements::kNeedTimeSignature |
                                           IProcessContextRequirements::kNeedBarPositionMusic)) != 0;
        const bool hasMeter = time.numerator > 0 && time.denominator > 0;
        const double tempo = time.tempo > 0.0 ? time.tempo : wantsTempo ? 120.0 : 0.0;
        const int numerator = hasMeter ? time.numerator : wantsMeter ? 4 : 0;
        const int denominator = hasMeter ? time.denominator : wantsMeter ? 4 : 0;

        if (tempo > 0.0)
        {
            context.tempo = tempo;
            context.projectTimeMusic = static_cast<double>(time.position) / context.sampleRate * tempo / 60.0;
            context.state |= ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
        }

        if (numerator > 0)
        {
            context.timeSigNumerator = numerator;
            context.timeSigDenominator = denominator;
            context.state |= ProcessContext::kTimeSigValid;

            if (tempo > 0.0)
            {
                const double bar = numerator * 4.0 / denominator;
                context.barPositionMusic = std::floor(context.projectTimeMusic / bar) * bar;
                context.state |= ProcessContext::kBarPositionValid;
            }
        }
    }

    bool run(int frames, const PluginTime& time)
    {
        outputChanges.clear();
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

        fillContext(time);
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
    std::vector<float> silence, scratchMain;
    bool passesOverEmpty = false;
    ChangeList inputChanges, outputChanges;
    EventList inputEvents, outputEvents;
    ProcessContext context{};
    std::optional<uint32> asked;
    PluginTime lastTime;
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

std::vector<PluginFolder> Vst3Format::folders() const
{
    std::vector<PluginFolder> folders;
    std::error_code error;
    const auto home = paths::systemFolder(paths::Folder::home, error);
#if defined(__APPLE__)
    folders.push_back({"/Library/Audio/Plug-Ins/VST3", false});
    if (!error)
        folders.push_back({home / "Library" / "Audio" / "Plug-Ins" / "VST3", true});
#elif defined(_WIN32)
    if (const auto common = system::environmentVariable("CommonProgramFiles"))
        folders.push_back({paths::fromUtf8(*common) / "VST3", false});
    if (const auto local = paths::systemFolder(paths::Folder::localAppData, error); !error)
        folders.push_back({local / "Programs" / "Common" / "VST3", true});
#else
    folders.push_back({"/usr/lib/vst3", false});
    folders.push_back({"/usr/local/lib/vst3", false});
    if (!error)
        folders.push_back({home / ".vst3", true});
#endif
    return folders;
}

bool Vst3Format::isPluginFile(const std::filesystem::path& path) const
{
    if (!symbolengine::text::equalsIgnoringCaseAscii(paths::toUtf8(path.extension()), ".vst3"))
        return false;
    std::error_code error;
#if defined(_WIN32)
    return std::filesystem::is_directory(path, error) || std::filesystem::is_regular_file(path, error);
#else
    return std::filesystem::is_directory(path, error);
#endif
}

namespace
{

// The binary in a bundle's architecture folder for this system, as "x86_64-win" or "aarch64-linux" names it.
[[maybe_unused]] std::string architectureFolder()
{
    const auto architecture = std::string(system::architecture());
#if defined(_WIN32)
    return (architecture == "x86" ? "x86" : architecture) + "-win";
#else
    return (architecture == "arm64" ? "aarch64" : architecture == "x86" ? "i386" : architecture) + "-linux";
#endif
}

}

std::optional<PluginFileFacts> Vst3Format::examine(const std::filesystem::path& file, PluginError& error) const
{
    error = {};
    std::error_code ignored;
    std::vector<std::filesystem::path> binaries;
    std::filesystem::path loaded;

    if (std::filesystem::is_directory(file, ignored))
    {
#if defined(__APPLE__)
        // The executable its Info.plist names, else one named as the bundle is, else the one file there.
        const auto folder = file / "Contents" / "MacOS";
        auto named = folder / file.stem();
        const auto plist = files::read(file / "Contents" / "Info.plist", ignored);
        if (const auto at = plist.find("<key>CFBundleExecutable</key>"); at != std::string::npos)
            if (const auto open = plist.find("<string>", at), close = plist.find("</string>", at);
                open != std::string::npos && close != std::string::npos && open < close)
                named = folder / paths::fromUtf8(plist.substr(open + 8, close - open - 8));
        if (std::filesystem::is_regular_file(named, ignored))
            loaded = named;
        else
            for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
                if (entry.is_regular_file(ignored))
                    loaded = entry.path();
        if (!loaded.empty())
            binaries.push_back(loaded);
#else
        // Each architecture folder's binary for this system, named as the bundle is.
    #if defined(_WIN32)
        const std::string system = "-win", suffix = ".vst3";
    #else
        const std::string system = "-linux", suffix = ".so";
    #endif
        for (const auto& entry : std::filesystem::directory_iterator(file / "Contents", ignored))
        {
            const auto name = paths::toUtf8(entry.path().filename());
            if (!entry.is_directory(ignored) || !name.ends_with(system))
                continue;
            auto binary = entry.path() / file.stem();
            binary += suffix;
            if (!std::filesystem::is_regular_file(binary, ignored))
                continue;
            binaries.push_back(binary);
            if (name == architectureFolder())
                loaded = binary;
        }
#endif
    }
    else if (std::filesystem::is_regular_file(file, ignored))
    {
        loaded = file;
        binaries.push_back(file);
    }

    if (binaries.empty())
    {
        error = {"It holds no binary for this system.", "read", 0};
        return std::nullopt;
    }
    if (loaded.empty())
        loaded = binaries.front();

    PluginFileFacts facts;
    std::error_code readError;
    facts.stamp.size = static_cast<std::int64_t>(std::filesystem::file_size(loaded, readError));
    facts.stamp.modified = pluginFiles::modifiedNanoseconds(loaded, readError);
    if (readError)
    {
        error = {"Its binary can't be read: " + readError.message(), "read", 0};
        return std::nullopt;
    }
    for (const auto& binary : binaries)
        for (const auto& architecture : pluginFiles::architecturesOf(binary))
            if (std::find(facts.architectures.begin(), facts.architectures.end(), architecture) ==
                facts.architectures.end())
                facts.architectures.push_back(architecture);

    // Its version as it declares it to the system, else as its manifest does, whose time also says it changed.
    facts.stamp.version = pluginFiles::declaredVersion(file, loaded);
    const auto manifest = file / "Contents" / "Resources" / "moduleinfo.json";
    if (std::error_code missing; std::filesystem::is_regular_file(manifest, missing))
    {
        facts.stamp.manifestModified = pluginFiles::modifiedNanoseconds(manifest, missing);
        if (facts.stamp.version.empty())
            if (const auto info = json::tryParse(files::read(manifest, missing)))
                facts.stamp.version = (*info)["Version"].stringOr("");
    }
    return facts;
}

std::string Vst3Format::name() const
{
    return "VST3";
}

namespace
{

// Text from one of the factory's fixed-size fields, which needn't end in a zero.
template <std::size_t size>
std::string fieldText(const char8 (&field)[size])
{
    return std::string(field, strnlen(field, size));
}

}

std::vector<PluginDescription> Vst3Format::pluginsIn(const std::filesystem::path& file, PluginError& error)
{
    error = {};
    const auto module = shared->module(file, error);
    if (module == nullptr)
        return {};

    auto* factory = module->factory();
    std::string vendor;
    if (PFactoryInfo info; factory->getFactoryInfo(&info) == kResultOk)
        vendor = fieldText(info.vendor);
    FUnknownPtr<IPluginFactory2> second(factory);
    FUnknownPtr<IPluginFactory3> third(factory);

    std::vector<PluginDescription> plugins;
    for (int32 index = 0; index < factory->countClasses(); ++index)
    {
        PClassInfo info;
        if (factory->getClassInfo(index, &info) != kResultOk || fieldText(info.category) != kVstAudioEffectClass)
            continue;

        char8 id[33] = {};
        FUID(info.cid).toString(id);
        PluginDescription plugin;
        plugin.identity = {"VST3", id};
        plugin.location = file;
        plugin.name = fieldText(info.name);
        plugin.maker = vendor;

        if (PClassInfo2 more; second && second->getClassInfo2(index, &more) == kResultOk)
        {
            if (const auto maker = fieldText(more.vendor); !maker.empty())
                plugin.maker = maker;
            plugin.version = fieldText(more.version);
            plugin.sdk = fieldText(more.sdkVersion);
            const auto categories = fieldText(more.subCategories);
            for (const auto category : symbolengine::text::split(categories, "|"))
                if (category == PlugType::kInstrument)
                    plugin.kind = PluginKind::instrument;
        }
        if (PClassInfoW wide; third && third->getClassInfoUnicode(index, &wide) == kResultOk)
            if (auto name = StringConvert::convert(wide.name); !name.empty())
                plugin.name = std::move(name);
        plugins.push_back(std::move(plugin));
    }
    return plugins;
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
