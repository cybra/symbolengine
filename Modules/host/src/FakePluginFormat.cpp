#include "symbolengine/host/FakePluginFormat.h"

#include "PluginFiles.h"

#include <symbolengine/core/Paths.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace symbolengine
{

namespace
{
class FakePlugin;
}

struct FakePluginFormat::Shared
{
    std::function<bool()> onMainThread;
    std::function<void(std::function<void()>)> post;

    mutable std::mutex lock;
    std::function<std::vector<PluginDescription>(const std::filesystem::path&, PluginError&)> files;
    std::map<std::string, Script> scripts;
    std::map<std::string, std::vector<std::string>> calls;
    std::vector<std::string> misuses;
    struct Live
    {
        std::string id;
        PluginFormatClient* client = nullptr;
        FakePlugin* plugin = nullptr;
    };

    std::vector<Live> live;
    bool allOnMain = true;

    void record(const std::string& id, std::string call)
    {
        const std::lock_guard held(lock);
        calls[id].push_back(std::move(call));
    }

    void misuse(const std::string& id, const std::string& what)
    {
        const std::lock_guard held(lock);
        misuses.push_back(id + ": " + what);
    }

    Script scriptFor(const std::string& id) const
    {
        const std::lock_guard held(lock);
        const auto found = scripts.find(id);
        return found == scripts.end() ? Script{} : found->second;
    }
};

namespace
{

class FakePlugin final : public PluginFormatInstance
{
public:
    FakePlugin(std::shared_ptr<FakePluginFormat::Shared> made, std::string name, FakePluginFormat::Script given,
               PluginFormatClient& listener)
        : shared(std::move(made)), id(std::move(name)), script(std::move(given)), client(listener)
    {
        for (const auto& parameter : script.parameters)
            heard[parameter.id] = parameter.defaultValue;

        const std::lock_guard held(shared->lock);
        shared->live.push_back({id, &client, this});
    }

    ~FakePlugin() override
    {
        const Main call(*this, "destroy", true);

        if (active)
            misuse("taken apart while active");

        if (editorOpen)
            misuse("taken apart with its editor open");

        {
            const std::lock_guard held(shared->lock);
            shared->allOnMain = shared->allOnMain && shared->onMainThread();
            const auto me =
                std::find_if(shared->live.begin(), shared->live.end(),
                             [this](const FakePluginFormat::Shared::Live& one) { return one.plugin == this; });

            if (me != shared->live.end())
                shared->live.erase(me);
        }
    }

    PluginLayout arrange(PluginLayout wanted) override
    {
        const Main call(*this, "arrange", true);

        if (active)
            misuse("arranged while active");

        if (script.takes.empty())
            return {0, 0};

        const auto taken = std::find(script.takes.begin(), script.takes.end(), wanted);
        layout = taken != script.takes.end() ? *taken : script.takes.front();
        return layout;
    }

    bool activate(const PluginSetup& setup, PluginError& error) override
    {
        const Main call(*this, "activate", true);

        if (active)
            misuse("activated while active");

        if (script.failToActivate || script.activations == 0)
        {
            error = {"The fake plugin was told not to activate.", "activate", 2};
            return false;
        }

        if (script.activations > 0)
            --script.activations;

        delay.assign(static_cast<std::size_t>(std::max(0, layout.outputs)),
                     std::vector<float>(static_cast<std::size_t>(std::max(1, script.latency)), 0.0f));
        delayAt = 0;
        maximumFrames = setup.maximumFrames;
        active = true;
        return true;
    }

    void deactivate() override
    {
        const Main call(*this, "deactivate", true);

        if (!active || processing)
            misuse("deactivated while not active or while processing");

        active = false;
    }

    int latency() override
    {
        const Main call(*this, "latency", false);

        if (!active)
            misuse("latency asked while not active");

        return script.latency;
    }

    int tail() override
    {
        const Main call(*this, "tail", false);

        if (!active)
            misuse("tail asked while not active");

        return script.tail;
    }

    void reset() override
    {
        const Main call(*this, "reset", true);

        if (!active || processing)
            misuse("reset while not active or while processing");

        for (auto& line : delay)
            std::fill(line.begin(), line.end(), 0.0f);
    }

    void startProcessing() override
    {
        const Audio call(*this, "startProcessing");

        if (!active || processing)
            misuse("started processing while not active or already processing");

        processing = true;
    }

    void stopProcessing() override
    {
        const Audio call(*this, "stopProcessing");

        if (!processing)
            misuse("stopped processing while not processing");

        processing = false;
    }

    bool process(PluginBlock& block) override
    {
        const Audio call(*this, "process");

        if (!processing)
            misuse("processed while not processing");

        if (block.frames > maximumFrames)
            misuse("given more frames than it was set up for");

        if (block.inputChannels != layout.inputs || block.outputChannels != layout.outputs)
            misuse("given other channels than its layout");

        if (script.onBlock != nullptr)
            script.onBlock(block);

        for (int i = 0, last = 0; i < block.changeCount; ++i)
        {
            if (block.changes[i].offset < last || block.changes[i].offset >= std::max(1, block.frames))
                misuse("given a change outside its block or out of order");

            last = block.changes[i].offset;
        }

        const auto length = static_cast<std::size_t>(std::max(1, script.latency));
        int next = 0;

        for (int i = 0; i < block.frames; ++i)
        {
            // Each change heard at its frame.
            for (; next < block.changeCount && block.changes[next].offset <= i; ++next)
                hear(&block.changes[next], 1);

            for (int c = 0; c < block.outputChannels; ++c)
            {
                const float in = block.inputChannels > 0
                                     ? block.inputs[std::min(c, block.inputChannels - 1)][i] * script.gain
                                     : 0.0f;
                auto& line = delay[static_cast<std::size_t>(c)];

                if (script.latency > 0)
                {
                    block.outputs[c][i] = line[delayAt];
                    line[delayAt] = in;
                }
                else
                {
                    block.outputs[c][i] = in;
                }
            }

            if (script.latency > 0)
                delayAt = (delayAt + 1) % length;
        }

        hear(block.changes + next, block.changeCount - next);
        block.reportedCount = 0;

        for (const auto& report : script.reports)
            if (block.reportedCount < block.reportedRoom)
                block.reported[block.reportedCount++] = report;

        return true;
    }

    void flush(const PluginParameterChange* changes, int count) override
    {
        const Main call(*this, "flush", true);

        if (!active || processing)
            misuse("flushed while not active or while processing");

        hear(changes, count);
    }

    std::optional<std::string> state(PluginStateUse, PluginError&) override
    {
        const Main call(*this, "state", false);
        std::string state = "fake state";

        for (const auto& [parameter, value] : heard)
            state += ";" + std::to_string(parameter) + "=" + std::to_string(value);

        return state;
    }

    std::vector<PluginParameter> parameters() override
    {
        const Main call(*this, "parameters", false);
        return script.parameters;
    }

    double parameterValue(std::uint32_t parameterId) override
    {
        const Main call(*this, "parameterValue", false);
        const std::lock_guard held(shownLock);
        const auto found = shown.find(parameterId);

        if (found != shown.end())
            return found->second;

        for (const auto& parameter : script.parameters)
            if (parameter.id == parameterId)
                return parameter.defaultValue;

        return 0.0;
    }

    void showParameterValue(std::uint32_t parameterId, double value) override
    {
        const Main call(*this, "showParameterValue", false);
        {
            const std::lock_guard held(shownLock);
            shown[parameterId] = value;
        }

        // Edited back to the host, as some plugins do.
        if (script.echoes > 0)
            client.edit({PluginEdit::Kind::value, parameterId,
                         script.echoes == 2 ? static_cast<double>(static_cast<float>(value)) : value});
    }

    // An edit of its own, from any thread, as its window makes: the value it shows follows, unless it forgets.
    void ownEdit(const PluginEdit& edit)
    {
        if (edit.kind == PluginEdit::Kind::value && !script.forgetsOwnEdits)
        {
            const std::lock_guard held(shownLock);
            shown[edit.id] = edit.value;
        }

        client.edit(edit);
    }

    std::string parameterText(std::uint32_t parameterId, double value) override
    {
        const Main call(*this, "parameterText", false);
        return std::to_string(parameterId) + ":" + std::to_string(value);
    }

    bool setState(std::string_view, PluginStateUse, PluginError&) override
    {
        const Main call(*this, "setState", true);

        if (processing)
            misuse("given its state while processing");

        return true;
    }

    bool hasEditor() override
    {
        const Main call(*this, "hasEditor", false);
        return script.editor;
    }

    std::unique_ptr<PluginFormatEditor> openEditor(void*, PluginEditorFrame& frame, PluginError& error) override;

    // An editor of the fake's: its calls recorded and checked as the plugin's own, one open at a time.
    class Editor final : public PluginFormatEditor
    {
    public:
        explicit Editor(FakePlugin& plugin) : fake(plugin), current(plugin.script.editorSize) {}

        ~Editor() override
        {
            const Main call(fake, "closeEditor", false);
            fake.editorOpen = false;
        }

        PluginEditorSize size() override { return current; }

        bool resizable() override
        {
            const Main call(fake, "resizable", false);
            return true;
        }

        PluginEditorSize constrain(PluginEditorSize wanted) override
        {
            const Main call(fake, "constrain", false);
            return wanted;
        }

        void resized(PluginEditorSize size) override
        {
            const Main call(fake, "resized", false);
            current = size;
        }

    private:
        FakePlugin& fake;
        PluginEditorSize current;
    };

private:
    // A call on the main thread: checked for its thread and for overlapping another call into the plugin, and, for
    // one that changes how it processes, for overlapping processing.
    struct Main
    {
        Main(FakePlugin& plugin, const char* name, bool exclusive) : fake(plugin), changes(exclusive)
        {
            fake.shared->record(fake.id, name);

            if (!fake.shared->onMainThread())
                fake.misuse(std::string(name) + " called off the main thread");

            if (++fake.mainCalls > 1)
                fake.misuse(std::string(name) + " overlapping another call on the main thread");

            if (changes)
                ++fake.exclusive;

            if (changes && fake.audioCalls > 0)
                fake.misuse(std::string(name) + " overlapping a call on the audio thread");

            // What the script makes it do, which may throw as a plugin's own code can.
            try
            {
                if (fake.script.inside != nullptr)
                    fake.script.inside(name);
            }
            catch (...)
            {
                leave();
                throw;
            }
        }

        void leave()
        {
            --fake.mainCalls;

            if (changes)
                --fake.exclusive;
        }

        ~Main() { leave(); }

        Main(const Main&) = delete;
        Main& operator=(const Main&) = delete;

        FakePlugin& fake;
        bool changes;
    };

    struct Audio
    {
        Audio(FakePlugin& plugin, const char* name) : fake(plugin)
        {
            if (std::string_view(name) != "process")
                fake.shared->record(fake.id, name);

            if (++fake.audioCalls > 1)
                fake.misuse(std::string(name) + " overlapping another call on the audio thread");

            if (fake.exclusive > 0)
                fake.misuse(std::string(name) + " overlapping a call that changes how it processes");

            // What the script makes it do, which may throw as a plugin's own code can.
            try
            {
                if (fake.script.inside != nullptr)
                    fake.script.inside(name);
            }
            catch (...)
            {
                --fake.audioCalls;
                throw;
            }
        }

        ~Audio() { --fake.audioCalls; }

        Audio(const Audio&) = delete;
        Audio& operator=(const Audio&) = delete;

        FakePlugin& fake;
    };

    void misuse(const std::string& what) { shared->misuse(id, what); }

    // Values its processing hears, from a block or a flush. Kept in a fixed map, sized when the plugin is made, so a
    // block never allocates.
    void hear(const PluginParameterChange* changes, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const auto found = heard.find(changes[i].id);

            if (found == heard.end())
            {
                misuse("given a change to a parameter it doesn't have");
                continue;
            }

            found->second = changes[i].value;

            if (changes[i].id == script.gainParameter)
                script.gain = static_cast<float>(changes[i].value);
        }
    }

    std::shared_ptr<FakePluginFormat::Shared> shared;
    std::string id;
    FakePluginFormat::Script script;
    PluginFormatClient& client;

    std::atomic<int> mainCalls{0}, audioCalls{0}, exclusive{0};
    std::atomic<bool> active{false}, processing{false};
    PluginLayout layout{};
    int maximumFrames = 0;
    std::vector<std::vector<float>> delay;
    std::size_t delayAt = 0;
    std::map<std::uint32_t, double> heard, shown;
    std::mutex shownLock;
    bool editorOpen = false;
};

std::unique_ptr<PluginFormatEditor> FakePlugin::openEditor(void*, PluginEditorFrame& frame, PluginError& error)
{
    const Main call(*this, "openEditor", false);

    if (!script.editor)
    {
        error = {"The fake plugin has no editor.", "openEditor", 0};
        return nullptr;
    }

    if (editorOpen)
        misuse("a second editor opened while one was open");

    editorOpen = true;
    frame.place(script.editorSize);
    return std::make_unique<Editor>(*this);
}

}

FakePluginFormat::FakePluginFormat(std::function<bool()> onMainThread, std::function<void(std::function<void()>)> post)
    : shared(std::make_shared<Shared>())
{
    shared->onMainThread = std::move(onMainThread);
    shared->post = std::move(post);
}

FakePluginFormat::~FakePluginFormat() = default;

void FakePluginFormat::script(const std::string& id, Script script)
{
    const std::lock_guard held(shared->lock);
    shared->scripts[id] = std::move(script);
}

void FakePluginFormat::edit(const std::string& id, const PluginEdit& edit)
{
    const std::lock_guard held(shared->lock);

    for (const auto& one : shared->live)
        if (one.id == id)
            one.plugin->ownEdit(edit);
}

void FakePluginFormat::notify(const std::string& id, PluginNotice what)
{
    const std::lock_guard held(shared->lock);

    for (const auto& one : shared->live)
        if (one.id == id)
            one.client->notice(what);
}

PluginDescription FakePluginFormat::describe(const std::string& id)
{
    PluginDescription plugin;
    plugin.identity = {"Fake", id};
    plugin.name = id;
    plugin.maker = "SymbolEngine";
    plugin.version = "1.0";
    return plugin;
}

std::vector<std::string> FakePluginFormat::calls(const std::string& id) const
{
    const std::lock_guard held(shared->lock);
    const auto found = shared->calls.find(id);
    return found == shared->calls.end() ? std::vector<std::string>{} : found->second;
}

std::vector<std::string> FakePluginFormat::misuses() const
{
    const std::lock_guard held(shared->lock);
    return shared->misuses;
}

int FakePluginFormat::alive() const
{
    const std::lock_guard held(shared->lock);
    return static_cast<int>(shared->live.size());
}

bool FakePluginFormat::allTakenApartOnMainThread() const
{
    const std::lock_guard held(shared->lock);
    return shared->allOnMain;
}

std::vector<PluginFolder> FakePluginFormat::folders() const
{
    return {};
}

bool FakePluginFormat::isPluginFile(const std::filesystem::path& path) const
{
    std::error_code error;
    return path.extension() == ".fake" && std::filesystem::is_regular_file(path, error);
}

std::optional<PluginFileFacts> FakePluginFormat::examine(const std::filesystem::path& file, PluginError& error) const
{
    std::error_code readError;
    PluginFileFacts facts;
    facts.stamp.size = static_cast<std::int64_t>(std::filesystem::file_size(file, readError));
    const auto time = std::filesystem::last_write_time(file, readError);
    if (readError)
    {
        error = {readError.message(), "read", 0};
        return std::nullopt;
    }
    facts.stamp.modified = pluginFiles::nanosecondsOf(time);
    error = {};
    return facts;
}

std::string FakePluginFormat::name() const
{
    return "Fake";
}

void FakePluginFormat::files(
    std::function<std::vector<PluginDescription>(const std::filesystem::path&, PluginError&)> holds)
{
    const std::lock_guard held(shared->lock);
    shared->files = std::move(holds);
}

std::vector<PluginDescription> FakePluginFormat::pluginsIn(const std::filesystem::path& file, PluginError& error)
{
    error = {};
    if (!shared->onMainThread())
        shared->misuse(paths::toUtf8(file), "its plugins listed off the main thread");

    decltype(shared->files) holds;
    {
        const std::lock_guard held(shared->lock);
        holds = shared->files;
    }
    if (holds == nullptr)
    {
        error = {"The fake format can't read files.", "read", 0};
        return {};
    }

    auto plugins = holds(file, error);
    for (auto& plugin : plugins)
        plugin.location = file;
    return plugins;
}

void FakePluginFormat::make(const PluginDescription& plugin, PluginFormatClient& client,
                            std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done)
{
    const auto& id = plugin.identity.id;
    shared->record(id, "make");

    if (!shared->onMainThread())
        shared->misuse(id, "made off the main thread");

    const auto script = shared->scriptFor(id);

    const auto finish = [held = shared, id, script, &client, done] {
        if (script.failToMake)
            done(nullptr, {"The fake plugin was told to fail.", "make", 1});
        else
            done(std::make_unique<FakePlugin>(held, id, script, client), {});
    };

    if (script.turnsToMake <= 0)
    {
        finish();
        return;
    }

    // Made over turns of the main thread, as a format that makes plugins asynchronously does.
    auto countdown = std::make_shared<std::function<void(int)>>();
    *countdown = [post = shared->post, finish, weak = std::weak_ptr(countdown)](int left) {
        if (left == 0)
            finish();
        else
            post([held = weak.lock(), left] { (*held)(left - 1); });
    };
    shared->post([countdown, turns = script.turnsToMake] { (*countdown)(turns - 1); });
}

}
