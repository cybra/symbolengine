#pragma once

#include "PluginFormat.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace symbolengine
{

/** A plugin format for tests, whose plugins do what a script says and check every call made into them: that each is
    on the right thread, in an order the formats allow, and never overlapping a call it mustn't. Its plugins pass their
    input to their output at a gain, delayed by their latency. */
class FakePluginFormat : public PluginFormat
{
public:
    struct Script
    {
        /** The layouts it takes; asked for another, it settles on the first. None means it has no buses at all. */
        std::vector<PluginLayout> takes{{2, 2}, {1, 1}, {1, 2}};

        int latency = 0;
        int tail = 0;
        float gain = 1.0f;

        /** Its parameters; a change to the one gainParameter names sets its gain from its next block. Its state holds
            each parameter's value as its processing has heard it. */
        std::vector<PluginParameter> parameters;
        std::uint32_t gainParameter = 0xFFFFFFFF;

        /** Whether its own edits leave the value it shows as it was, as some plugins' do. */
        bool forgetsOwnEdits = false;

        /** Whether it edits a parameter back while it is shown a value for it: 0 not, 1 with the same value, 2 with
            the value rounded to a float. */
        int echoes = 0;

        /** Values its processing reports every block, by parameter. */
        std::vector<PluginParameterChange> reports;

        /** Shown every block it is given, on the audio thread. */
        std::function<void(const PluginBlock&)> onBlock;

        /** Fails to be made, or to be activated, saying so. */
        bool failToMake = false;
        bool failToActivate = false;

        /** How many times it activates before it refuses as failToActivate does, as a plugin that can't be prepared
            again; any number while negative. */
        int activations = -1;

        /** How many turns of the main thread making it takes before it says it is made; 0 says so before make returns. */
        int turnsToMake = 0;

        /** Whether it has an editor, which opens at this size, placing its view through the frame first. */
        bool editor = false;
        PluginEditorSize editorSize{400, 300};

        /** Runs inside every call into the plugin, with the call's name and the thread it is on, where a test makes it
            misbehave: send notices, turn the run loop, wait on a lock, change the locale, throw. */
        std::function<void(std::string_view call)> inside;
    };

    /** The fake checks its main-thread calls against onMainThread, and turns of the main thread through post. */
    FakePluginFormat(std::function<bool()> onMainThread, std::function<void(std::function<void()>)> post);
    ~FakePluginFormat() override;

    /** What the plugin with this identity's id does; one with no script does the default. */
    void script(const std::string& id, Script script);

    /** Tells the host of a change in every live plugin with this id, from any thread, as plugins do. */
    void notify(const std::string& id, PluginNotice what);

    /** Edits a parameter in every live plugin with this id, from any thread, as a plugin's own window does, the value
        it shows following unless its script says it forgets. */
    void edit(const std::string& id, const PluginEdit& edit);

    /** The identity of a fake plugin with this id, as the host is to be given it. */
    static PluginDescription describe(const std::string& id);

    /** What each file holds for pluginsIn, as the function says, or the error it sets; without one, no file can be
        read. */
    void files(std::function<std::vector<PluginDescription>(const std::filesystem::path&, PluginError&)> holds);

    /** Every call made into plugins with this id, in order, by name. */
    std::vector<std::string> calls(const std::string& id) const;

    /** Every call made on the wrong thread, out of order or overlapping another it mustn't, said in words. */
    std::vector<std::string> misuses() const;

    /** How many of its plugins are alive. */
    int alive() const;

    /** Whether the main thread was the one each of its plugins was taken apart on. */
    bool allTakenApartOnMainThread() const;

    std::string name() const override;

    /** No folders of its own; its plugin files end in ".fake" and are stamped by their own size and time. */
    std::vector<PluginFolder> folders() const override;
    bool isPluginFile(const std::filesystem::path& path) const override;
    std::optional<PluginFileFacts> examine(const std::filesystem::path& file, PluginError& error) const override;
    std::vector<PluginDescription> pluginsIn(const std::filesystem::path& file, PluginError& error) override;
    void make(const PluginDescription& plugin, PluginFormatClient& client,
              std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done) override;

    struct Shared;

private:
    std::shared_ptr<Shared> shared;
};

}
